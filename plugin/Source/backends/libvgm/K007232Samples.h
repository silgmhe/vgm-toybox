#pragma once

#include <vector>

#include "playback/WaveExport.h"

namespace toybox::midi
{
	struct SongIr;
}

namespace toybox::backends::libvgm
{
	/** Every sample the song's K007232 voices ever started.

		The easiest of the sampled chips so far, for two reasons. There is no sample table: the
		driver writes a start address and the data says where it ends - `k007232.c:246` restarts the
		voice at the first byte whose **bit 7** is set. And the format is not compressed:
		`(value & 0x7F) - 0x40` is the level, seven-bit unsigned with `0x40` as silence.

		What it does have is the **bank**, and it matters. A voice reads
		`bank + (start & 0x1FFFF)`, the bank being register 0x14 for voice 0 and 0x15 for voice 1.
		Measured on `02 Title Demo`: `0x14 = 1` and `0x15 = 3`, which land on the ROM blocks at
		0x20000 and 0x60000 - the two voices read different 128 KB windows of one image. Ignoring the
		bank would make both voices play whichever window sits at zero.

		**The rate is per note.** This chip has no single sample rate: a voice consumes
		`(clock / 128) * 32 / (0x1000 - step)` bytes per second, so the same bytes started at two
		different steps are two different files. `chipRateHz` is the device's own `clock / 128` (the
		rate it reports), and the step factor is applied per sample.
	*/
	std::vector<playback::DecodedSample> collectK007232Samples (const midi::SongIr& ir,
																double chipRateHz);
}
