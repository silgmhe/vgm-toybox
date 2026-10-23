"""Turns Dn-FamiTracker's text export into the same note lines `--ftm-dump` prints.

This is the authoritative half of the comparison. ftm_reference.py is an independent reader of the
binary; this reads what FamiTracker itself says the module contains, so a disagreement here is the
loader being wrong rather than two readers sharing a mistake.

    Dn-FamiTracker.exe <module>.ftm -export <module>.txt
    python tools/ftm_from_text.py <module>.txt > reference.txt
    build/plugin/vgm_chip_test.exe --ftm-dump <module>.ftm > ours.txt

Only pitch channels are emitted: FamiTracker prints a noise channel's note as a noise-period index
(`A-#`), which is not a pitch and cannot be compared with one.

The walk matches the loader's rules - speed carries across frames, Fxx takes effect on its own row,
Bxx/Dxx/halt move the frame pointer, Sxx ends a note early - because the point is to compare how the
two sides *read the file*, not to redefine the timing here.
"""
import re
import sys

NOTE_RE = re.compile(r'^([A-G])([-#])(\d)$')
NOISE_RE = re.compile(r'^[0-9A-F]-#$')
ROW_RE = re.compile(r'^ROW ([0-9A-F]+) : (.*)$')
ORDER_RE = re.compile(r'^ORDER ([0-9A-F]+) : (.*)$')


def parse(path):
    pattern_length = speed = 0
    order = []
    patterns = {}
    current = None

    with open(path, encoding='utf-8', errors='replace') as handle:
        for line in handle:
            line = line.rstrip('\n')

            if line.startswith('TRACK '):
                parts = line.split()
                pattern_length = int(parts[1])
                speed = int(parts[2])
                continue

            match = ORDER_RE.match(line)
            if match:
                order.append([int(v, 16) for v in match.group(2).split()])
                continue

            if line.startswith('PATTERN '):
                current = int(line.split()[1], 16)
                patterns[current] = {}
                continue

            match = ROW_RE.match(line)
            if match and current is not None:
                row = int(match.group(1), 16)
                cells = [cell.strip() for cell in match.group(2).split(':')]
                patterns[current][row] = cells

    return pattern_length, speed, order, patterns


def cell_pitch(cell):
    """The MIDI note a cell asks for, or None when the cell is empty, noise or a marker."""
    token = cell.split()[0] if cell.split() else '...'
    if token == '...':
        return None
    match = NOTE_RE.match(token)
    if not match:
        return None                     # '===' release, '---' cut, 'N-#' noise
    letter, accidental, octave = match.group(1), match.group(2), int(match.group(3))
    semitone = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}[letter]
    if accidental == '#':
        semitone += 1
    return octave * 12 + semitone + 12


def cell_effects(cell):
    tokens = cell.split()
    return [t for t in tokens[3:] if t != '...']


def walk(pattern_length, speed, order, patterns, channels):
    notes = []
    frame = 0
    base = 0
    steps = 0
    limit = len(order) * 4 + 64

    while 0 <= frame < len(order) and steps < limit:
        steps += 1
        next_frame = frame + 1
        offsets = [0]
        clock = speed
        frame_speed = speed

        for row in range(pattern_length):
            for channel in channels:
                cells = patterns.get(order[frame][channel], {}).get(row)
                if not cells or channel >= len(cells):
                    continue
                for effect in cell_effects(cells[channel]):
                    if effect.startswith('F') and len(effect) == 3:
                        value = int(effect[1:], 16)
                        if value > 0:
                            clock = value
            offsets.append(offsets[-1] + clock)
        frame_speed = clock

        for channel in channels:
            rows = patterns.get(order[frame][channel], {})
            if not rows:
                continue

            hits = []
            for row in sorted(rows):
                if row >= len(offsets):
                    continue
                cells = rows[row]
                if channel >= len(cells):
                    continue
                pitch = cell_pitch(cells[channel])
                if pitch is not None:
                    hits.append((row, pitch, cells[channel]))

            for index, (row, pitch, cell) in enumerate(hits):
                start = base + offsets[row]
                end = (base + offsets[hits[index + 1][0]] if index + 1 < len(hits)
                       else base + offsets[pattern_length])

                # Sxx ends the note where the tracker ends it
                for cut_row in sorted(rows):
                    if cut_row < row or cut_row >= len(offsets):
                        continue
                    cells = rows[cut_row]
                    if channel >= len(cells):
                        continue
                    for effect in cell_effects(cells[channel]):
                        if effect.startswith('S') and len(effect) == 3:
                            end = min(end, base + offsets[cut_row] + (int(effect[1:], 16) & 0x0F))
                if end <= start:
                    continue
                notes.append((channel, pitch, start, end))

            for row in sorted(rows):
                if channel >= len(rows[row]):
                    continue
                for effect in cell_effects(rows[row][channel]):
                    if len(effect) != 3:
                        continue
                    kind, param = effect[0], int(effect[1:], 16)
                    if kind == 'B':
                        next_frame = param
                    elif kind == 'D':
                        next_frame = frame + 1 + param
                    elif kind == 'H':
                        next_frame = -1

        speed = frame_speed
        if next_frame <= frame:
            next_frame = -1
        base += offsets[pattern_length]
        frame = next_frame

    return notes


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return

    channels = [0, 1, 2]
    if '--channels' in sys.argv:
        index = sys.argv.index('--channels')
        channels = [int(v) for v in sys.argv[index + 1].split(',')]
        del sys.argv[index:index + 2]

    for path in sys.argv[1:]:
        pattern_length, speed, order, patterns = parse(path)
        notes = walk(pattern_length, speed, order, patterns, channels)
        print('module %s' % path.replace('\\', '/').split('/')[-1])
        for channel, pitch, start, end in sorted(notes, key=lambda n: (n[0], n[2], n[1])):
            print('note track=%d pitch=%d start=%d end=%d velocity=0' % (channel, pitch, start, end))


if __name__ == '__main__':
    main()
