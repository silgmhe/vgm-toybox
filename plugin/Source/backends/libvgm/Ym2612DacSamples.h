#pragma once

#include <vector>

#include "playback/WaveExport.h"

namespace toybox::midi
{
struct SongIr;
}

namespace toybox::backends::libvgm
{

/** The YM2612's DAC material: the PCM bank the sixth channel plays, as one file.

	This chip is the odd one out among the sample players. The others name a range of a ROM with
	registers; the YM2612 is handed one byte at a time at `0x2A`, and the driver decides where those
	bytes came from - a `0x67` PCM block streamed by `0x80-0x8F`, or the Z80's own memory. The file
	carries no sample table, only a stream of bytes with timings.

	**One file, not one per hit.** Cutting the stream at pauses was tried and produced fragments
	(3 ms to 165 ms, eleven of them on `02 Green Hill Zone`) that were not the drums a listener would
	name; the boundaries belong to the driver, and the bank is the material the file actually carries.

	**The rate is the median interval that follows a non-silence byte.** Not the mean over the whole
	stream: the driver feeds silence at 1..2 ticks and drums at 5..6, so the two differ by more than
	two to one, and a mean lands between them and matches neither. The median over the loud bytes is
	22 050 Hz on `02 Green Hill Zone` (2 ticks), which is where the drum bytes actually sit. A median
	rather than a mean so that a handful of long gaps cannot drag it down.

	The bytes are unsigned around `0x80` (`SampleFormat::pcm8Unsigned`), which is what the register
	takes.
*/
std::vector<playback::DecodedSample> collectYm2612DacSamples (const midi::SongIr& ir);

} // namespace toybox::backends::libvgm
