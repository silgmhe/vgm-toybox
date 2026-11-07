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
	/** The wave tables every keyed VSU voice points at.

		**There is no ROM here.** A VSU wave is five tables of thirty-two six-bit entries, and they
		arrive as ordinary register writes to the low half of the address space (`vsu.c:229`), so the
		collector rebuilds them from the write stream rather than from a data block. The core refuses
		those writes while any voice is on (`vsu.c:222-228`), which is why every table a song uses is
		complete before the first key-on.

		**A file here is one cycle, not one sample.** Thirty-two entries are the whole wave, and the
		rate is how many entries the voice walks per second - `clock / (2048 - freq)` (`vsu.c:266`).
		A file therefore loops seamlessly at the pitch the row reports.

		**Two address spaces share one low byte.** The VGM offset's bit 8 separates them (below 0x100
		is wave RAM, 0x100..0x17F the channel registers), and the loader keeps it in `port`; a reader
		that dropped it made a wave upload look like a control-register write.
	*/
	std::vector<playback::DecodedSample> collectVsuSamples (const midi::SongIr& ir, uint32_t clockHz);
}
