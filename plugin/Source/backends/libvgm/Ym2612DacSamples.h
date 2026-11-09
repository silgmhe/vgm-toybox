#pragma once

#include <vector>

#include "playback/WaveExport.h"

namespace toybox::midi
{
struct SongIr;
}

namespace toybox::backends::libvgm
{

/** The YM2612's DAC material, one file per region the driver seeks to.

	This chip is the odd one out among the sample players. The others name a range of a ROM with
	registers; the YM2612 is handed one byte at a time at `0x2A`, and the driver decides where those
	bytes came from - a `0x67` PCM block streamed by `0x80-0x8F`, or the Z80's own memory.

	**The bank is a table, not a sample.** A driver moves the read pointer with `0xE0`, and the
	distinct offsets it uses are its sample table. Each entry is written at one fixed rate: measured
	across four Sonic 1 songs, a visit to one offset averages 1.90 ticks and to another 5.47, and the
	same offset never varies by more than 0.02 ticks between visits. A single rate for the whole song
	therefore matched at best the half of the bank that happened to run at that rate - which is why a
	per-song constant (13 799, then 22 050, then 22 050 x 1.637) never generalised.

	So: **one file per region**, starting at each `0xE0` offset and running to the next one; **rate =
	the mean interval inside that region**. The mean, because the intervals are dithered - a stream
	that wants 5.5 ticks arrives as 5 and 6, and a median would quantise it to 8 820.

	A song with no seeks (or no bank) falls back to one file for the whole bank or stream, at the mean
	interval of its loud bytes.
*/
std::vector<playback::DecodedSample> collectYm2612DacSamples (const midi::SongIr& ir);

} // namespace toybox::backends::libvgm
