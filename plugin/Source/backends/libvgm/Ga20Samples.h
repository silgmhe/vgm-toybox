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
	/** Every sample the song's GA20 voices ever started.

		The simplest of the sampled chips so far, because there is nothing to look up. A driver
		writes a start address and a control byte, and the data says where the sample ends:
		`iremga20.c:174` stops playback at the first **0x00** byte. The format is eight-bit unsigned
		with `0x80` as silence (`rom[pos] - 0x80`, `iremga20.c:177`).

		What the chip *does* offer is a length register (2 and 3), and reading it would be wrong:
		every driver writes it and the core never reads it - playback runs until the zero byte.

		**The rate is the device's own.** `iremga20.c:367` starts it at `clock / 4`; one byte lasts
		`256 - rate` of those samples. `rateHz` is what the running device reports, and the arithmetic
		here is only the fallback for a song whose device never started.

		There is no bank and only ever one GA20 in a VGM (the header has a single clock field for it),
		so a sample is named by its address alone - unlike the K007232, whose two voices read
		different windows of one image.
	*/
	std::vector<playback::DecodedSample> collectGa20Samples (const midi::SongIr& ir, double rateHz,
															uint32_t clockHz);
}
