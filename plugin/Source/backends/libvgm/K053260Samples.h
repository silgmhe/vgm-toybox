#pragma once

#include <vector>

#include "playback/WaveExport.h"

namespace toybox::midi
{
	struct SongIr;
}

namespace toybox::backends::libvgm
{
	/** Every PCM sample a K053260 song plays, in the order the drivers first started them.

		The first sampled chip whose registers say how long a sample is: a start address and a length,
		not a start and an end. The catch is that the length starts one byte late - the core's first
		fetch is the byte *after* the start address - and getting that wrong shifts every sample by
		one byte, which is a step in front of the waveform rather than an obviously broken one.

		KADPCM voices are not collected: they are four-bit deltas and `SampleFormat` has no value for
		them. The decoder can still draw them, because measuring a pitch needs the same integration.
	*/
	std::vector<playback::DecodedSample> collectK053260Samples (const midi::SongIr& ir);
}
