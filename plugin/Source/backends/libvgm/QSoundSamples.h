#pragma once

#include <vector>

#include "playback/WaveExport.h"

namespace toybox::midi
{
	struct SongIr;
}

namespace toybox::backends::libvgm
{
	/** Every sample a QSound song plays, in the order the drivers first started them.

		The chip names a sample three ways at once: a start address and an end address inside a
		sixty-four-kilobyte bank, and the bank itself - which lives on the *previous* voice's register
		row, not its own. Reading any one of them from the wrong place gives a plausible address and
		the wrong bytes, so this walks the same register map the decoder does.
	*/
	std::vector<playback::DecodedSample> collectQSoundSamples (const midi::SongIr& ir);
}
