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
	/** **Every wave table a WonderSwan channel played**, one file per distinct table.

		The tables live in the chip's 16 KB of RAM, uploaded through `0xC6` and located by `0x8F`: a
		sixteen-byte table per channel, sixteen bytes apart, read at `clock / (2048 - f)` entries a
		second. `0x90`'s per-channel bits are the key on.

		Each table is expanded to one 4-bit level per byte before it is added, because `prom4` reads a
		level per byte; the expansion keeps the chip's nibble order (an **even** step takes the
		**low** nibble), so the file and the row draw the same shape.
	*/
	std::vector<playback::DecodedSample> collectWonderswanSamples (const midi::SongIr& ir,
																   uint32_t clockHz);
}
