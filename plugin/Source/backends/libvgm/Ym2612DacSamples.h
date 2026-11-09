#pragma once

#include <vector>

#include "playback/WaveExport.h"

namespace toybox::midi
{
struct SongIr;
}

namespace toybox::backends::libvgm
{

/** The YM2612's DAC hits.

	This chip is the odd one out among the sample players. The others name a range of a ROM with
	registers; the YM2612 is handed one byte at a time at `0x2A`, and the driver decides where those
	bytes came from - a `0x67` PCM block streamed by `0x80-0x8F`, or the Z80's own memory. The file
	therefore carries no sample table, only a stream of bytes with timings.

	**The boundary is the pause.** Between drums the driver streams `0x80` (silence); a quiet run of
	2 ms or more separates one hit from the next. Measured on `02 Green Hill Zone` (447 541 bytes):
	2 ms yields 428 hits and 11 distinct recordings, the most repeated 95 times; 8 ms merges them
	into 142 hits of which 6 are distinct, and 1 ms shatters each decay into pieces.

	**The rate is the hit's own.** The driver writes silence quickly and drum bytes at the sample's
	own speed, so the mean over the whole stream (13.8 kHz) is 1.7x too fast for a drum body; the
	mean inside one hit is 6.9..20 kHz, most around 7.9 kHz. Pauses longer than 64 ticks are left out
	of that mean, because they are the space between drums, not part of one.

	The bytes are unsigned around `0x80` (`SampleFormat::pcm8Unsigned`), which is what the register
	takes. Two hits with identical bytes are one file: the same drum is replayed dozens of times.
*/
std::vector<playback::DecodedSample> collectYm2612DacSamples (const midi::SongIr& ir);

} // namespace toybox::backends::libvgm
