#pragma once

#include <vector>

#include "playback/WaveExport.h"

namespace toybox::midi
{
	struct SongIr;
}

namespace toybox::backends::libvgm
{
	/** Every sample the song's OKIM6295 voices ever started, one entry per sample number.

		Simpler than the C140's counterpart in one way and harder in another. Simpler because the
		chip keeps its own table: `command * 8` in the ROM holds six bytes of start and stop, so a
		sample number is all the registers ever carry - nothing in a register says where the sample
		is. Harder because those addresses count **nibbles**, not bytes, and the data is a delta
		codec rather than levels, so the export has to run the same decoder the row uses
		(`chips/Okim6295Codec.h`) instead of handing out the bytes as they are.

		The rate is the chip's own: one fixed oscillator, halved by the ROM board's pin 7 when the
		song writes register 0x0C with bit 0 set. Two levels share every byte, so the level rate is
		twice the byte rate.
	*/
	std::vector<playback::DecodedSample> collectOkim6295Samples (const midi::SongIr& ir);
}
