#!/usr/bin/env python3
"""Compares two renders of the same song by their loudness envelope.

Peak and RMS say a file has sound; they say nothing about whether the notes arrive in the right
place. An envelope - RMS in short windows - does: two renderers that agree on the music put their
peaks and gaps at the same times, whatever their overall gain.

This exists because FamiTracker cannot export a WAV (its command line has an empty .wav branch), so
the reference has to be rendered from its .nsf export by nsf2wav, which is built from the same
NSFPlay sources the plugin uses. Usage:

    python tools/wav_envelope_compare.py ours.wav reference.wav [seconds] [window_ms]
"""

import array
import math
import sys
import wave


def envelope(path, seconds, window_seconds):
    handle = wave.open(path, "rb")
    channels = handle.getnchannels()
    width = handle.getsampwidth()
    rate = handle.getframerate()
    frames = min(int(rate * seconds), handle.getnframes())
    raw = handle.readframes(frames)
    handle.close()

    if width not in (2, 4):
        raise SystemExit("unsupported sample width: %d bytes" % width)

    samples = array.array("h" if width == 2 else "i")
    samples.frombytes(raw)

    mono = samples[0::channels]
    window = max(1, int(rate * window_seconds))

    result = []
    for start in range(0, len(mono) - window, window):
        chunk = mono[start:start + window]
        total = 0.0
        for value in chunk:
            total += float(value) * float(value)
        result.append(math.sqrt(total / window))

    return rate, result


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)

    ours_path, reference_path = sys.argv[1], sys.argv[2]
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 30.0
    window = float(sys.argv[4]) / 1000.0 if len(sys.argv) > 4 else 0.05

    ours_rate, ours = envelope(ours_path, seconds, window)
    ref_rate, reference = envelope(reference_path, seconds, window)

    count = min(len(ours), len(reference))
    if count < 10:
        raise SystemExit("not enough audio to compare")

    ours = ours[:count]
    reference = reference[:count]

    ours_mean = sum(ours) / count
    ref_mean = sum(reference) / count

    covariance = sum((a - ours_mean) * (b - ref_mean) for a, b in zip(ours, reference))
    spread = math.sqrt(sum((a - ours_mean) ** 2 for a in ours)
                       * sum((b - ref_mean) ** 2 for b in reference))

    correlation = covariance / spread if spread else 0.0

    print("windows    : %d (%.0f ms each, %.1f s)" % (count, window * 1000.0, seconds))
    print("sample rate: ours %d, reference %d" % (ours_rate, ref_rate))
    print("mean level : ours %.1f, reference %.1f (ratio %.2f)"
          % (ours_mean, ref_mean, ours_mean / ref_mean if ref_mean else 0.0))
    print("correlation: %.3f" % correlation)

    # The window-by-window disagreement, so a poor score points somewhere instead of being a number.
    worst = sorted(((abs(a - b), i, a, b) for i, (a, b) in enumerate(zip(ours, reference))),
                   reverse=True)[:5]

    for difference, index, a, b in worst:
        print("  t=%6.2f s  ours %7.1f  reference %7.1f  delta %7.1f"
              % (index * window, a, b, a - b))


if __name__ == "__main__":
    main()
