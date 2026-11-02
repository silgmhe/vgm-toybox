#pragma once

#include <vector>

#include "playback/WaveExport.h"

namespace toybox::midi
{
	struct SongIr;
}

namespace toybox::backends::libvgm
{
	/** Every PCM sample a K054539 song plays, in the order the drivers first started them.

		The fourth way these chips name a sample, and the strangest: no length and no end address, a
		*marker* instead - the core stops a voice when the unit it just fetched is `0x80` (eight-bit),
		`0x8000` (sixteen-bit) or the byte `0x88` (four-bit DPCM). The distance to that marker is the
		sample, found by scanning, which is exact because the ROM is here.

		All three formats are exported, each through its own `SampleFormat`. Eight-bit and sixteen-bit
		are levels and differ only in width. Four-bit DPCM is deltas: summed into levels, then
		normalised to the sample's own peak, because its steps are small enough that dividing by a
		sixteen-bit full scale leaves the row flat against the centre line - drawn, and impossible to
		see. That was reported as "the drum channels are a straight line" and is why `dpcm4` exists.
	*/
	std::vector<playback::DecodedSample> collectK054539Samples (const midi::SongIr& ir);
}
