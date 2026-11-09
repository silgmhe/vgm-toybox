#pragma once

#include <vector>

#include "playback/WaveExport.h"

namespace toybox::midi
{
struct SongIr;
}

namespace toybox::backends::libvgm
{

/** The YM2612's DAC material: the PCM bank the sixth channel plays.

	This chip is the odd one out among the sample players. The others name a range of a ROM with
	registers; the YM2612 is handed one byte at a time at `0x2A`, and the driver decides where those
	bytes come from. Two shapes appear in VGM files, and they are handled differently:

	1. **A PCM data block** (`0x67` type `0x00`, or `0x80` for the ROM-tagged form). The bytes are
	   carried by the file, and the `0x80-0x8F` commands stream them into the DAC - that is how
	   `02 Green Hill Zone` works: 13 132 bytes, played at 8 495 Hz, sought into by 228 `0xE0`
	   commands. **This is the case worth exporting**, and the unit is the bank: the file carries a
	   loop of drum material, not a table of one-shots, so cutting it into "samples" would be a guess.

	2. **No block at all.** The driver feeds bytes from the Z80's own RAM; all the file has is the
	   `0x2A` stream. There the only structure is the pauses: a gap of 30 ms or more separates one
	   hit from the next, and those runs are exported instead.

	The rate is measured from the stream in both cases - the YM2612's DAC has no rate register, and
	the same sample is played at different speeds to transpose it. It is the mean interval between
	consecutive writes (ignoring pauses), so it is the rate the song actually played at rather than a
	nominal 8 kHz.

	The bytes are unsigned around `0x80` (`SampleFormat::pcm8Unsigned`), which is what the register
	takes.
*/
std::vector<playback::DecodedSample> collectYm2612DacSamples (const midi::SongIr& ir);

} // namespace toybox::backends::libvgm
