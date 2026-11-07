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
	/** **One file per voice, and for these rips that means one file per song.**

		The sample ROM arrives as data block `0x86` and each voice names a start and a stop in nibbles.
		What the drivers here do is *stream*: voice 0's range is rewritten forward as the song plays,
		so the honest summary of a voice is the union of every range it ever named - one continuous
		recording, which is what the ear hears.

		Voices the song never uses produce nothing: their level is written to zero once and their
		start and stop stay at zero, so there is no range to export.
	*/
	std::vector<playback::DecodedSample> collectYmz280bSamples (const midi::SongIr& ir,
																uint32_t clockHz);
}
