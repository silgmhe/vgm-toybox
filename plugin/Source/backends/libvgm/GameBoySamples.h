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
	/** **Every wave table the Game Boy's wave channel played**, one file per distinct table.

		The DMG has exactly one such channel: sixteen bytes at 0xFF30-0xFF3F, written through `0xB3`
		offsets 0x20-0x2F and read at `clock / (2 * (2048 - f))` entries a second. The trigger
		(`0x0E` bit 7) is the key on, and it only sounds while the APU is powered and the DAC is on.

		Like the WonderSwan's, the table is expanded to one 4-bit level per byte so `prom4` can read
		it - with the **high** nibble first, which is this chip's order and the opposite of the other.
	*/
	std::vector<playback::DecodedSample> collectGameBoySamples (const midi::SongIr& ir,
																uint32_t clockHz);
}
