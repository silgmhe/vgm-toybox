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
	/** The eight waveforms each K005289 voice can point at.

		**The bytes come from the song's `0xC3` block, not from the register writes.** It is a RAM
		block with a sixteen-bit address (`_VGM_RAM_CHIPS`), 512 bytes: two channels of eight
		waveforms of thirty-two four-bit steps (`k005289.c:59`, `195-197`). The registers only choose
		which of the eight a voice plays - `waveform` is bits 5-7 of the control write - so the
		collector pairs one static image with the register stream.

		**A file is one cycle, not a sample.** Thirty-two entries are the whole wave and the voice
		walks them at `clock / (freq + 1)` entries per second (`k005289.c:190`), so a file loops at
		the pitch the row reports. The pitch register arrives inverted, which the walk below undoes
		the same way the decoder does.
	*/
	std::vector<playback::DecodedSample> collectK005289Samples (const midi::SongIr& ir,
																uint32_t clockHz);
}
