#pragma once

#include <vector>

#include "playback/WaveExport.h"

namespace toybox::midi
{
	struct SongIr;
}

namespace toybox::backends::libvgm
{
	/** Every sample the song's C140 voices ever pointed at, decoded once each.

		The counterpart of `collectNesDpcmSamples`, and the same idea: a chip's registers name where
		its samples live, so walking the writes finds them without playing the song. What differs is
		where the bytes are - the 2A03 keeps them in a RAM block, the C140 in ROM blocks - and how a
		voice names one: a bank and a sixteen-bit address here, a page and a length there.

		A voice that names a mu-law sample is skipped. Its bytes are logarithmically coded, so a level
		per byte would be a shape the chip never makes; the row still draws its rate, and the export
		says nothing rather than something wrong.

		The rate written is the chip's reference rate, not the voice's. Every voice plays its sample
		at its own speed, so there is no single "the" rate for a file that holds many - and the
		reference is what a sampler reading one sample out of context should be handed.
	*/
	std::vector<playback::DecodedSample> collectC140Samples (const midi::SongIr& ir);
}
