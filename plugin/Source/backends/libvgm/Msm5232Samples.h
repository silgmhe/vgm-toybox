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
	/** The staircase each MSM5232 voice emits.

		**No memory here either.** The chip has no wave RAM: a voice is four fifty-percent squares
		taken from one counter (msm5232.c:305-320), and the group control register decides which of
		them the group drives. So the shape is a function of two numbers a song writes down - the
		pitch index (which picks the divider) and the group's enable nibble - and `chips/Msm5232Shape.h`
		rebuilds it with the same code the decoder draws with.

		The pitch index is a key: `note = 21 + index`, eighty-eight entries from A0, so a file's rate
		is `hz * points` - how many ticks make one period.
	*/
	std::vector<playback::DecodedSample> collectMsm5232Samples (const midi::SongIr& ir,
																uint32_t clockHz);
}
