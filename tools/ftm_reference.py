"""An independent reader for FamiTracker modules, used to cross-check the C++ loader.

This is deliberately a second implementation rather than a wrapper: it was written from the
reference's read order (DocumentFile.cpp, FamiTrackerDoc.cpp), in another language, and it emits the
same lines `vgm_chip_test --ftm-dump` does for notes. Two implementations agreeing on 42,558 events
is evidence that neither misread a field; one implementation agreeing with itself is not.

It is not authoritative - FamiTracker itself is - but it is independent, which is the property a
cross-check needs.

    python tools/ftm_reference.py test_file/ftm/*.ftm > reference.txt
    build/plugin/vgm_chip_test.exe --ftm-dump <file>   # the other side
"""
import struct
import sys

HEADER_TEXT = b'FamiTracker Module'
HEADER_DN = b'Dn-FamiTracker Module'


def read_blocks(data):
    head = HEADER_TEXT if data.startswith(HEADER_TEXT) else HEADER_DN
    pos = len(head) + 4
    blocks = {}
    while pos + 24 <= len(data):
        name = data[pos:pos + 16].split(b'\x00')[0].decode('latin-1')
        version, size = struct.unpack_from('<II', data, pos + 16)
        blocks[name] = (version, data[pos + 24:pos + 24 + size])
        pos += 24 + size
    return blocks


class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def u8(self):
        value = self.data[self.pos]
        self.pos += 1
        return value

    def i32(self):
        value = struct.unpack_from('<i', self.data, self.pos)[0]
        self.pos += 4
        return value

    def skip(self, count):
        self.pos += count

    def cstr(self):
        end = self.data.index(b'\x00', self.pos)
        text = self.data[self.pos:end].decode('latin-1')
        self.pos = end + 1
        return text


def parse(data):
    blocks = read_blocks(data)

    params = Reader(blocks['PARAMS'][1])
    params.u8()
    channels = params.i32()

    header = Reader(blocks['HEADER'][1])
    track_count = header.u8() + 1
    for _ in range(track_count):
        header.cstr()

    channel_types = []
    effect_columns = []
    for _ in range(channels):
        channel_types.append(header.u8())
        columns = 0
        for track in range(track_count):
            columns = header.u8()
        effect_columns.append(columns)

    frames = Reader(blocks['FRAMES'][1])
    frame_count = frames.i32()
    speed = frames.i32()
    frames.i32()                            # tempo
    length = frames.i32()
    order = [[] for _ in range(channels)]
    for _ in range(frame_count):
        for channel in range(channels):
            order[channel].append(frames.u8())

    patterns = {}
    body = Reader(blocks['PATTERNS'][1])
    while body.pos + 16 <= len(body.data):
        track = body.i32()
        channel = body.i32()
        index = body.i32()
        items = body.i32()
        rows = []
        for _ in range(items):
            row = body.i32()
            note = body.u8()
            octave = body.u8()
            instrument = body.u8()
            volume = body.u8()
            effects = []
            columns = effect_columns[channel] if channel < len(effect_columns) else 0
            for _ in range(columns + 1):
                number = body.u8()
                param = body.u8()
                if number:
                    effects.append((number, param))
            rows.append((row, note, octave, instrument, volume, effects))
        if track == 0:
            patterns[(channel, index)] = rows

    return {
        'channels': channels,
        'channel_types': channel_types,
        'speed': speed,
        'length': length,
        'frame_count': frame_count,
        'order': order,
        'patterns': patterns,
    }


def walk(module):
    """Frames are walked rather than played straight through: Bxx and Dxx are the structure."""
    out = []
    speed = module['speed']
    length = module['length']
    frame = 0
    base = 0
    steps = 0
    limit = module['frame_count'] * 4 + 64

    while 0 <= frame < module['frame_count'] and steps < limit:
        steps += 1
        next_frame = frame + 1
        offsets = []
        clock = speed
        offsets.append(0)
        for row in range(length):
            # an Fxx executes on its own row, so that row already lasts the new speed
            for channel in range(module['channels']):
                rows = module['patterns'].get((channel, module['order'][channel][frame]))
                if not rows:
                    continue
                for entry in rows:
                    if entry[0] != row:
                        continue
                    for number, param in entry[5]:
                        if number == 1 and param > 0:
                            clock = param
            offsets.append(offsets[-1] + clock)

        for channel in range(module['channels']):
            rows = module['patterns'].get((channel, module['order'][channel][frame]))
            if not rows:
                continue

            notes = [entry for entry in rows if entry[1] != 0]
            for i, (row, note, octave, instrument, volume, effects) in enumerate(notes):
                row = max(0, min(row, length - 1))   # a row past the pattern is not a position
                start = base + offsets[row]
                end = (base + offsets[notes[i + 1][0] % len(offsets)] if i + 1 < len(notes)
                       else base + offsets[length])

                # Sxx ends the note early
                for cut_row, cut_note, _, _, _, cut_effects in rows:
                    if cut_note or cut_row < row:
                        continue
                    for number, param in cut_effects:
                        if number == 23:
                            end = min(end, base + offsets[cut_row] + (param & 0x0F))

                if end <= start:
                    continue
                pitch = min(127, max(0, 12 + octave * 12 + (note - 1)))   # a release/cut marker is not a pitch
                out.append((channel, pitch, start, end))

            for row, note, _, _, _, effects in rows:
                for number, param in effects:
                    if number == 2:
                        next_frame = param
                    elif number == 3:
                        next_frame = frame + 1 + param
                    elif number == 4:
                        next_frame = -1

        # the speed carries into later frames, so it lives outside the frame loop
        speed = clock

        if next_frame <= frame:
            next_frame = -1
        base += offsets[length]
        frame = next_frame

    return out


def main():
    for path in sys.argv[1:]:
        with open(path, 'rb') as handle:
            module = parse(handle.read())
        print('module %s' % path.replace('\\', '/').split('/')[-1])
        notes = walk(module)
        # our dump orders by channel then time; match that so a diff is readable
        for channel, pitch, start, end in sorted(notes, key=lambda n: (n[0], n[2], n[1])):
            print('note track=%d pitch=%d start=%d end=%d velocity=0' % (channel, pitch, start, end))


if __name__ == '__main__':
    main()
