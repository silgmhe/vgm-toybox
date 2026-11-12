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
	/** **Every wave table a HuC6280 voice played**, one file per distinct table.

		The counterpart of `collectVsuSamples` for the PC Engine: thirty-two 5-bit entries per channel,
		loaded through register 0x06 while the channel is silent (the chip's index only advances when
		it is off), and read at `clock / f` entries a second.

		A file is one cycle, so it carries `SampleFormat::huc5` and the step rate - the same two
		numbers the decoder's row reports, which is what keeps the file and the picture from drifting.
	*/
	std::vector<playback::DecodedSample> collectHuC6280Samples (const midi::SongIr& ir, uint32_t clockHz);
}
