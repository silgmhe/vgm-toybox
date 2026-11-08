#pragma once

#include <cstdint>
#include <vector>

#include "midi/SongIr.h"
#include "playback/WaveExport.h"

namespace toybox::backends::libvgm
{
	/** **ADPCM-A: six sample voices over one ROM.** They sit on the chip's *second* register port
		(`0x59` on a YM2610, `0x57` on a YM2608) at offsets 0x00..0x2D: `0x00` keys the six voices
		(bit 7 stops them all), `0x10+c`/`0x18+c` are a voice's start pages, `0x20+c`/`0x28+c` its
		end pages, `0x08+c` its level.

		Every voice reads the same `0x82` image - a 8 MB bank whose blocks arrive already placed - so
		the range a key-on latches is enough to cut a file. Decoding is `SampleFormat::adpcmA4`.
	*/
	std::vector<playback::DecodedSample> collectOpnAdpcmSamples (const midi::SongIr& ir,
																 uint32_t clockHz);
}
