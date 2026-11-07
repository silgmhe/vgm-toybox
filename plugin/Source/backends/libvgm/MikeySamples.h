#pragma once

#include <cstdint>
#include <vector>

#include "playback/WaveExport.h"

namespace toybox::midi
{
	struct SongIr;
}

namespace toybox::backends::libvgm
{
	/** The patterns Mikey's four channels generate.

		**There is nothing to read out of memory here.** A Mikey channel is a twelve-bit shift
		register clocked by its timer, and the output is either the register's new bit as ± volume or
		an accumulation of those steps (`mikey.c:490-503`). The shape is therefore a property of
		(seed, taps, volume, integrate) and is rebuilt by walking the same recurrence the chip does -
		`chips/MikeyPattern.h`, shared with the decoder so there is one answer to what a register
		write sounds like.

		**A file is one cycle of that pattern, not a sample.** Its length is the LFSR period: two
		steps for the alternating pattern a plain tone uses, four for the taps these drivers pick,
		and past `kMaxWavePoints` the channel is noise and produces no file at all.
	*/
	std::vector<playback::DecodedSample> collectMikeySamples (const midi::SongIr& ir,
															 uint32_t clockHz);
}
