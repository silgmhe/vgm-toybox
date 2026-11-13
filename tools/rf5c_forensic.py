#!/usr/bin/env python3
"""**A read-only look at what an RF5C164 actually plays, byte by byte.**

Design 9.5 shelved this chip because eleven attempts each looked decisive and each was overturned, and
the last readings had the chip's RAM at 0x8000 holding `80 80 80 80` while our assembled image had
`99 19 83 97`. That is not a paradox if the chip plays a *rolling buffer*: this file uploads 155 KB into
a 64 KB RAM and then streams 512-byte pages in while the song runs, so a static snapshot cannot say what
was playing at 0:08 - only a timeline can.

So this replays the stream in order, rebuilding the RAM as the writes arrive, and at every key-on takes
the bytes the voice starts on **from the RAM as it stood at that moment**. It only reads the file.

Usage:
    python tools/rf5c_forensic.py <file.vgz> [--limit 20]

Per hit it prints the address, the step (and the rate it means), the length measured to the first
`0xFF` - the chip's loop/stop marker - and the first four bytes. The summary counts how many *distinct*
byte ranges the hits name, which is what an export would produce.
"""

import argparse
import collections
import gzip
import struct
import sys


RAM_BYTES = 64 * 1024
CHIP_RATE = 32552.0          # the Sega CD's PCM clock: one byte per `step` of 2048 per this many ticks
STEP_PER_BYTE = 2048.0

# Register offsets, from `scd_pcm.c:222-330`. 0x07 selects a channel (bit 6 set) or a RAM bank (clear),
# 0x08 is the per-channel on/off, and 0x06 is the start page - 256-byte units, `St_Addr = Data << 8`.
REG_STEP_LOW = 0x02
REG_STEP_HIGH = 0x03
REG_LOOP_LOW = 0x04
REG_LOOP_HIGH = 0x05
REG_START = 0x06
REG_CONTROL = 0x07
REG_CHANNELS = 0x08


class Voice:
    """What the chip holds for one of its eight channels."""

    def __init__(self):
        self.step = 0
        self.start_page = 0
        self.loop = 0
        self.enabled = False


def read_header(raw):
    version = struct.unpack_from('<I', raw, 8)[0]
    offset = struct.unpack_from('<I', raw, 0x34)[0]
    data_offset = 0x34 + offset if offset else 0x40
    end = 0x04 + struct.unpack_from('<I', raw, 0x04)[0]
    return version, data_offset, min(end, len(raw))


def bytes_from(ram, start, loop):
    """The bytes a voice starting at `start` reads until the chip's `0xFF` marker.

    `scd_pcm.c:378-390` compares each byte with 0xFF and jumps to `Loop_Addr` when it matches; when the
    loop point itself holds 0xFF the channel stops, which is why the walk here stops at the marker
    instead of following the loop. Measured with a wrap, because the core's address register is 27 bits
    while the RAM is 64 KB - a sample with no marker in range runs off the end of the image.
    """
    length = 0

    while length < RAM_BYTES:
        if ram[(start + length) & (RAM_BYTES - 1)] == 0xFF:
            break

        length += 1

    body = bytes(ram[(start + i) & (RAM_BYTES - 1)] for i in range(length))
    return length, body


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('path')
    parser.add_argument('--limit', type=int, default=16, help='how many distinct ranges to print')
    parser.add_argument('--at', type=float, default=None,
                        help='also print the hits within half a second of this song time')
    parser.add_argument('--period', default=None,
                        help='report the strongest byte periods of the range starting at this hex address')
    args = parser.parse_args()

    with gzip.open(args.path, 'rb') as handle:
        raw = handle.read()

    version, pos, end = read_header(raw)
    print(f"=== {args.path.split('/')[-1].split(chr(92))[-1]}  version {version:04X}  data {pos:#x}")

    ram = bytearray(RAM_BYTES)
    voices = [Voice() for _ in range(8)]
    blocks = collections.Counter()
    pages = 0
    hits = []                       # (time, channel, start, length, rate, body)
    time = 0
    chip_channel = 0
    ram_bank = 0

    def take_hit(channel, voice):
        start = (voice.start_page << 8) & (RAM_BYTES - 1)
        length, body = bytes_from(ram, start, voice.loop)
        rate = voice.step * CHIP_RATE / STEP_PER_BYTE if voice.step else 0.0
        hits.append((time, channel, start, voice.loop, length, rate, body))

    while pos < end:
        cmd = raw[pos]

        if cmd == 0x66:
            break

        if cmd == 0x67:
            kind = raw[pos + 2]
            size = struct.unpack_from('<I', raw, pos + 3)[0] & 0x7FFFFFFF
            blocks[kind] += 1

            if kind == 0x02:
                # The load-time PCM bank. 155 KB in this file; the chip's RAM keeps the first 64 KB,
                # because `SCD_PCM_MemBlockWrite` clamps to `RAMSize` (`scd_pcm.c:454-457`).
                payload = raw[pos + 7:pos + 7 + size]
                ram[0:min(len(payload), RAM_BYTES)] = payload[:RAM_BYTES]
            elif kind == 0xC1:
                # A page written while the song runs: a two-byte address, then the bytes.
                # **The RAM bank is or-ed into every page address** - libvgm does it in
                # `DoRAMOfsPatches` (`dataOfs |= _rf5cBank[..] << 12`), and a tool that skips it
                # builds an image whose bytes sit 4 KB away from the chip's.
                address = struct.unpack_from('<H', raw, pos + 7)[0] | (ram_bank << 12)
                payload = raw[pos + 9:pos + 7 + size]
                pages += 1

                for i, byte in enumerate(payload):
                    if address + i < RAM_BYTES:
                        ram[address + i] = byte

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

        if cmd == 0xB1:
            offset = raw[pos + 1] & 0x7F
            value = raw[pos + 2]
            pos += 3

            if offset == REG_CONTROL:
                if (value & 0x40) != 0:
                    chip_channel = value & 0x07
                else:
                    ram_bank = value & 0x0F

                continue

            voice = voices[chip_channel]

            if offset == REG_STEP_LOW:
                voice.step = (voice.step & 0xFF00) | value
            elif offset == REG_STEP_HIGH:
                voice.step = (voice.step & 0x00FF) | (value << 8)
            elif offset == REG_LOOP_LOW:
                voice.loop = (voice.loop & 0xFF00) | value
            elif offset == REG_LOOP_HIGH:
                voice.loop = (voice.loop & 0x00FF) | (value << 8)
            elif offset == REG_START:
                voice.start_page = value

                # A start written while the channel already plays restarts it from there
                # (`scd_pcm.c:302-310`: the reset only happens when the channel is off, but the
                # address register is rebuilt from `St_Addr` on the next enable).
                if voice.enabled:
                    take_hit(chip_channel, voice)
            elif offset == REG_CHANNELS:
                for i in range(8):
                    on = (value & (1 << i)) == 0
                    was_on = voices[i].enabled

                    if on and not was_on:
                        take_hit(i, voices[i])

                    voices[i].enabled = on

            continue

        # Every other command: skip by its length.
        length = 0

        if 0x50 <= cmd <= 0x5F or 0xB0 <= cmd <= 0xBF:
            length = 2
        elif 0xC0 <= cmd <= 0xDF:
            length = 3
        elif cmd in (0xE0, 0x90, 0x91, 0x95, 0xE1):
            length = 4
        elif cmd == 0x92:
            length = 5
        elif cmd == 0x93:
            length = 10
        elif cmd == 0x94:
            length = 1
        elif cmd == 0x68:
            length = 11

        pos += 1 + length

    print(f"    blocks {dict(blocks)}  runtime RAM pages {pages}  hits {len(hits)}")

    # The hit carries the state as it was when the voice started, loop point included: reading it off
    # the channel afterwards would report whatever the song happened to leave there.
    distinct = collections.OrderedDict()

    for hit in hits:
        _, channel, start, loop, length, rate, body = hit
        distinct.setdefault((start, loop, length, body), []).append((hit[0], channel, rate))

    print(f"    distinct (start, loop, length, bytes) {len(distinct)}")

    for (start, loop, length, body), uses in list(distinct.items())[:args.limit]:
        first = ' '.join(f'{b:02X}' for b in body[:4])
        print(f"    start=0x{start:04X} loop=0x{loop:04X} len={length:5d} {uses[0][2]:7.0f}Hz  "
              f"x{len(uses):3d}  first={first}  at {uses[0][0] / 44100.0:6.2f}s")

    if args.period is not None:
        report_periods(hits, int(args.period, 16),
                       args.at * 44100.0 if args.at is not None else 0.0)

    if args.at is not None:
        # What the chip was reading at a chosen moment, next to what our final image holds there. The
        # two disagreeing is the whole reason 9.5 exists, so both are printed.
        centre = args.at * 44100.0
        print(f"    --- hits within 0.5 s of {args.at:.2f}s, and the final image at the same address")

        for hit in hits:
            if abs(hit[0] - centre) > 0.5 * 44100.0:
                continue

            _, channel, start, loop, length, rate, body = hit
            final = bytes(ram[start + i] if start + i < RAM_BYTES else 0xFF for i in range(8))
            now = ' '.join(f'{b:02X}' for b in body[:8])
            later = ' '.join(f'{b:02X}' for b in final)
            print(f"    t={hit[0] / 44100.0:6.2f}s ch={channel} start=0x{start:04X} loop=0x{loop:04X} "
                  f"len={length:5d} {rate:7.0f}Hz  then=[{now}]  final=[{later}]")


def report_periods(hits, start, centre):
    """The byte periods a range repeats at, as a shape rather than a level.

    A voiced sample repeats every `period` bytes, so `rate / period` is the pitch it sounds at. This is
    the number the 9.5 argument needed: the export was said to repeat at 12 bytes while the render's
    pitch implied 146, and no amount of address arithmetic could settle which was right.
    """
    # The hit nearest the moment asked for: the buffer is rewritten as the song runs, so the
    # first hit at an address is usually not the note being discussed.
    candidates = [hit for hit in hits if hit[2] == start]
    chosen = min(candidates, key=lambda hit: abs(hit[0] - centre)) if candidates else None

    if chosen is None:
        print(f"    no hit starts at 0x{start:04X}")
        return

    # Only the head of the range: a 32 KB buffer holds several instruments, and averaging over all of
    # them reports the noise floor instead of the note.
    body = chosen[6][:8192]
    rate = chosen[5]
    values = [byte - 0x80 for byte in body]
    best = []

    for lag in range(2, 513):
        if lag >= len(values):
            break

        total = 0.0
        for i in range(0, len(values) - lag, 3):        # every third byte: enough, and fast
            total += values[i] * values[i + lag]

        best.append((total / (len(values) - lag), lag))

    best.sort(reverse=True)
    print(f"    range 0x{start:04X}: {len(body)} bytes at {rate:.0f}Hz, strongest periods:")

    for score, lag in best[:4]:
        print(f"      {lag:4d} bytes -> {rate / lag:7.1f} Hz  (correlation {score:9.0f})")


if __name__ == '__main__':
    sys.exit(main())
