#pragma once

#include <vector>

#include "playback/WaveExport.h"

namespace toybox::midi
{
struct SongIr;
}

namespace toybox::backends::libvgm::dac
{
struct Table;
}

namespace toybox::backends::libvgm
{

/** The YM2612's DAC material, one file per hit the driver actually played.

	This chip is the odd one out among the sample players. The others name a range of a ROM with
	registers; the YM2612 is handed one byte at a time at `0x2A`, and the driver decides where those
	bytes came from - a `0x67` PCM block streamed by `0x80-0x8F`, or the Z80's own memory.

	**A hit is a run of bytes, not a range of the bank.** The `0xE0` offsets are where the driver
	moves the read pointer; they are not sample boundaries, and the driver routinely plays straight
	through one into the next (Marble's `0x0A80` runs 1 693 bytes across two later offsets). So the
	unit is what the reference implementation (`vgm2fur`) uses: the bytes between one `0xE0` and the
	next, or between two halves of a play separated by a pause of 11.6 ms or more.

	The rate is **`tickRate * length / duration`** over that run - an integer division, so the number
	in our WAV header equals the number in a `.fur` sample for the same run. The first measured rate
	wins for a repeated `(start, length)`, again to match.

	A per-song rate never generalised (13 799, then 22 050, then 22 050 x 1.637); the reason is that
	the same song plays some entries at 8 kHz and others at 23 kHz. A per-*region* rate was closer but
	still blended runs that cross an offset - Labyrinth's offset 0 measured 14.9 kHz where the runs
	from it are 23.1 kHz.

	**What the file cannot say**, recorded so the next reader does not go looking: which entry is a kick
	and which a snare (offsets, not names), where an entry loops (the driver jumps back with no marker),
	whether a rate was changed to transpose a drum (it only shows as a different run rate), and whether
	a short run is a stop-tone rather than a sample (Green Hill's `0x0000` has 93- and 96-byte ones).
	**The file name carries the sample's number** - `ym2612dac_07_...` - and that number is the one the
	row shows as a note (`dac::noteForSample`, C4 + the index). The payload is `pcm8Signed`, so it
	matches a `.fur` sample byte for byte.

	`table` is the song's shared table (`dac::buildTable`): passing it avoids a second scan over every
	stream byte and guarantees the list matches the runs the row draws. Null builds one here, which is
	what a caller outside the backend gets.

	See `docs/memo/2026-11-10-the-dac-is-a-latch.md`,
	`docs/research/2026-11-10-vgm2fur-dac-runs.md` and
	`docs/desgin/ym2612-dac-visualisation-and-export.md`.
*/
std::vector<playback::DecodedSample> collectYm2612DacSamples (const midi::SongIr& ir,
														  const dac::Table* table = nullptr);

} // namespace toybox::backends::libvgm
