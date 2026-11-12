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
	/** **Every wave table an SCC voice played**, one file per distinct table.

		Five channels of thirty-two signed bytes each, written through the `0xD2` latch pair the loader
		already splits into two register writes, and read at `2 * clock / (f + 1)` entries a second.

		`plusMode` is the K052539 layout: channel 4 keeps its own half at 0x80-0x9F instead of sharing
		channel 3's. It is a runtime fact (`devCfg->flags`, the VGM clock's bit 31) rather than an IR
		one, which is why it arrives as an argument like the clock does.
	*/
	std::vector<playback::DecodedSample> collectSccSamples (const midi::SongIr& ir, uint32_t clockHz,
															bool plusMode);
}
