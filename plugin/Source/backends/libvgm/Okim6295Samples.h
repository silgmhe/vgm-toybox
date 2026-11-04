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
	/** Every sample the song's OKIM6295 voices ever started, one entry per sample number.

		Simpler than the C140's counterpart in one way and harder in another. Simpler because the
		chip keeps its own table: `command * 8` in the ROM holds six bytes of start and stop, so a
		sample number is all the registers ever carry - nothing in a register says where the sample
		is. Harder because the data is a delta codec rather than levels, so the export has to run
		the same decoder the row uses (`chips/Okim6295Codec.h`) instead of handing out the bytes.

		**The addresses are bytes, and the sample is twice as many nibbles.** `okim6295.c` sets
		`base_offset = start` and `count = 2 * (stop - start + 1)`, reading `base + sample / 2` -
		so a byte carries two levels and the count is in nibbles.

		**The rate is the one the device reports.** That core starts at `master_clock / divisor`,
		one nibble per output sample, and the divisor is the board's pin-7 strap (132 or 165) -
		the same ratio this file once guessed and got 1.25x wrong. `rateHz` is therefore
		`DEV_INFO::sampleRate`, passed in by the caller; `clockHz` is only the fallback for a
		caller that could not resolve it.
	*/
	std::vector<playback::DecodedSample> collectOkim6295Samples (const midi::SongIr& ir,
																 double rateHz, uint32_t clockHz);
}
