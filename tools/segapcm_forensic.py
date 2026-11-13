#!/usr/bin/env python3
"""**A read-only look at what a SegaPCM driver actually writes.**

**Do not share code with the product parser** (`plugin/Source/backends/libvgm/VgmCommandReader.cpp`).
This tool's value is being an independent implementation: when the two disagree, the product is the
one to doubt first. Refactoring this to reuse the product's parser would make it agree by construction
and destroy the only thing it is for.

Design 9.16.2 asks for this before any model of the chip's start / loop / end is changed: the
registers are the playhead, but which write belongs to which hit - and what `end` really points at -
was never confirmed against the stream. So this prints the writes, in order, with the values the
chip derives from them, and never writes anything itself.

Usage:
    python tools/segapcm_forensic.py <file.vgz> [--seconds 20] [--channel 7] [--all] [--summary]

What it prints for every `0xC0`:
    t=  12.345s ch= 7 reg=0x86 val=0x02  start=0x5437C loop=0x50000 end=0x60000 pitch=66 @16113Hz  KEY-ON

`start` is the ROM byte index the voice will read (bank included), `loop` and `end` are the
registers converted the way `segapcm.c:156` converts them, and the rate is
`pitch * clock / 32768` bytes a second. The flags on the right call out the events the collector
cares about: KEY-ON (control bit 0 cleared), STOP, START-MOVED (0x84/0x85 rewritten while
playing), VOLUME (while playing), END/PITCH (while playing).

The summary counts those events, so "how often does a driver retrigger by moving the address?"
and "does `end` ever land on a sample boundary?" become numbers instead of guesses.
"""

import argparse
import collections
import gzip
import struct
import sys


# Register names, from the comment block at the top of `segapcm.c:130`.
LOW_NAMES = {
    0x00: '?0',
    0x01: '?1',
    0x02: 'volL',
    0x03: 'volR',
    0x04: 'loopLo',
    0x05: 'loopHi',
    0x06: 'end',
    0x07: 'pitch',
}
HIGH_NAMES = {
    0x00: '?80',
    0x01: '?81',
    0x02: '?82',
    0x03: '?83',
    0x04: 'addrMid',
    0x05: 'addrHigh',
    0x06: 'control',
    0x07: '?87',
}


class Voice:
    """What the chip holds for one voice; the same eight registers the core reads."""

    def __init__(self):
        self.vol_l = 0
        self.vol_r = 0
        self.loop_lo = 0
        self.loop_hi = 0
        self.end = 0
        self.pitch = 0
        self.addr_mid = 0
        self.addr_high = 0
        self.control = 0xFF     # the core resets its RAM to 0xFF (`segapcm.c:255`)
        self.playing = False


def read_header(raw):
    def le32(offset):
        return struct.unpack_from('<I', raw, offset)[0]

    data_offset = 0x34 + le32(0x34) if le32(0x34) else 0x40
    end = 0x04 + le32(0x04) if le32(0x04) else len(raw)

    return {
        'version': le32(0x08),
        'data_offset': data_offset,
        'end': min(end, len(raw)),
        'clock': le32(0xC0) if len(raw) > 0xC3 else 0,
        'shift': raw[0x3C] if len(raw) > 0x3E else 0,
        'mask': (raw[0x3E] if len(raw) > 0x3E and raw[0x3E] else 0x70),
    }


def command_length(cmd):
    """Bytes after the command byte, for the commands a SegaPCM song can contain."""
    if cmd in (0x61, 0x62, 0x63) or 0x70 <= cmd <= 0x8F:
        return 1 if cmd in (0x61, 0x62) else 0
    if 0x50 <= cmd <= 0x5F or 0xA0 <= cmd <= 0xBF:
        return 2
    if 0xC0 <= cmd <= 0xDF:
        return 3      # 16-bit address (big endian, bit 15 = chip) and a data byte
    if cmd == 0xE0:
        return 4
    if cmd in (0x90, 0x91, 0x95, 0xE1):
        return 4
    if cmd == 0x92:
        return 5
    if cmd == 0x93:
        return 10
    if cmd == 0x94:
        return 1
    if cmd == 0x68:
        return 11
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('path')
    parser.add_argument('--seconds', type=float, default=30.0, help='stop after this much time')
    parser.add_argument('--channel', type=int, default=-1, help='only this voice')
    parser.add_argument('--all', action='store_true', help='print every write, not just events')
    parser.add_argument('--summary', action='store_true', help='summary only')
    args = parser.parse_args()

    with gzip.open(args.path, 'rb') as handle:
        raw = handle.read()

    header = read_header(raw)

    # The header field this reads is only a clock in the versions that have one; anything outside
    # 1-100 MHz is a neighbour's bytes, and 8 MHz is the part every SegaPCM board used.
    # A 1.51 header stops at 0x80 and carries no SegaPCM clock, so the field read above is a
    # neighbour's bytes there. 4 MHz is what the System 16 and X boards ran the part at, and the
    # exported files' own rates (`pitch * clock / 32768`) confirm it for these songs.
    clock = header['clock'] if 1000000 < header['clock'] < 100000000 else 4000000

    print(f"=== {args.path.split('/')[-1].split(chr(92))[-1]}")
    print(f"    version {header['version']:04X}  clock {clock}  bank shift/mask "
          f"{header['shift']}/{header['mask']:02X}  data {header['data_offset']:#x}")

    voices = collections.defaultdict(dict)      # chip -> channel -> Voice
    stats = collections.Counter()
    time = 0
    pos = header['data_offset']
    end = header['end']
    last_time = -1.0

    def voice_of(chip, channel):
        table = voices[chip]

        if channel not in table:
            table[channel] = Voice()

        return table[channel]

    while pos < end:
        cmd = raw[pos]

        if cmd == 0x66:
            break

        if cmd == 0x67:
            size = struct.unpack_from('<I', raw, pos + 3)[0] & 0x7FFFFFFF
            pos += 7 + size
            continue

        if cmd == 0x61:
            time += raw[pos + 1]
            pos += 2
            continue

        if cmd == 0x62:
            time += raw[pos + 1] | (raw[pos + 2] << 8)
            pos += 3
            continue

        if cmd == 0x63:
            time += 882
            pos += 1
            continue

        if 0x70 <= cmd <= 0x7F:
            time += (cmd & 0x0F) + 1
            pos += 1
            continue

        if 0x80 <= cmd <= 0x8F:
            time += cmd & 0x0F
            pos += 1
            continue

        if cmd == 0xC0:
            # **Little endian**, unlike the X1-010's big-endian offset: the reader puts the low
            # byte in `addr` and the high byte in `port` (`VgmCommandReader.cpp`, the 0xC0 branch),
            # so the register number is `(port << 8) | addr` - 0x00-0x7F per voice, 0x80-0xFF the
            # current-address and control window.
            address = ((raw[pos + 2] << 8) | raw[pos + 1]) & 0x7FFF
            chip = 0
            value = raw[pos + 3]
            pos += 4

            channel = (address & 0x7F) >> 3
            high_window = (address & 0x80) != 0

            if channel >= 16:
                continue

            voice = voice_of(chip, channel)
            which = address & 7
            name = (HIGH_NAMES if high_window else LOW_NAMES)[which]

            flags = []
            seconds = time / 44100.0

            if not high_window:
                if which == 0x02:
                    voice.vol_l = value
                    if voice.playing:
                        flags.append('VOLUME')
                        stats['volume while playing'] += 1
                elif which == 0x03:
                    voice.vol_r = value
                    if voice.playing:
                        flags.append('VOLUME')
                        stats['volume while playing'] += 1
                elif which == 0x04:
                    voice.loop_lo = value
                elif which == 0x05:
                    voice.loop_hi = value
                elif which == 0x06:
                    voice.end = value
                    if voice.playing:
                        flags.append('END while playing')
                        stats['end while playing'] += 1
                elif which == 0x07:
                    voice.pitch = value
                    if voice.playing:
                        flags.append('PITCH while playing')
                        stats['pitch while playing'] += 1
            else:
                if which == 0x04:
                    if voice.playing and value != voice.addr_mid:
                        flags.append('START-MOVED')
                        stats['start moved while playing'] += 1

                    voice.addr_mid = value
                elif which == 0x05:
                    if voice.playing and value != voice.addr_high:
                        flags.append('START-MOVED')
                        stats['start moved while playing'] += 1

                    voice.addr_high = value
                elif which == 0x06:
                    was_off = (voice.control & 0x01) != 0
                    voice.control = value

                    if (value & 0x01) == 0 and was_off:
                        voice.playing = True
                        flags.append('KEY-ON')
                        stats['key-on'] += 1
                    elif (value & 0x01) != 0:
                        if voice.playing:
                            flags.append('STOP')

                        voice.playing = False

            offset = (voice.control & header['mask']) << header['shift']
            start = offset | ((voice.addr_high << 8) | voice.addr_mid)
            loop = offset | ((voice.loop_hi << 8) | voice.loop_lo)
            stop = offset | ((voice.end + 1) << 8)
            rate = voice.pitch * clock / 32768.0

            interesting = bool(flags) or args.all

            if (interesting and seconds <= args.seconds and last_time != seconds
                    and (args.channel < 0 or args.channel == channel) and not args.summary):
                print(f"    t={seconds:8.3f}s ch={channel:2d} {name:<8} = {value:02X}"
                      f"  start={start:07X} loop={loop:07X} end={stop:07X}"
                      f" pitch={voice.pitch:3d} @{rate:7.0f}Hz"
                      + (('  ' + ' '.join(flags)) if flags else ''))

            if flags:
                last_time = seconds

            continue

        pos += 1 + command_length(cmd)

    print("    --- summary ---")

    for name in ('key-on', 'start moved while playing', 'volume while playing',
                 'end while playing', 'pitch while playing'):
        print(f"    {name:<28} {stats[name]}")

    # What the end registers name, as a fraction of 64 KB: `end = 0xFF` means the whole bank.
    ends = collections.Counter()

    for table in voices.values():
        for voice in table.values():
            ends[voice.end] += 1

    if ends:
        print(f"    distinct end registers   {len(ends)}  e.g. "
              + ', '.join(f"{value:02X}" for value, _ in ends.most_common(6)))


if __name__ == '__main__':
    sys.exit(main())
