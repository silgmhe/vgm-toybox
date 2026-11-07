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
	/** Every sample the song's YMW258 voices ever started.

		The first sampled chip here whose **length is in a table** rather than in the data: the ROM
		begins with a twelve-byte entry per sample index (`multipcm.c:281-298`), holding the start
		address, the loop point and - as its two's complement negation - the number of samples. That
		is why this collector does not scan for a terminator and why a sample with no loop still has
		an exact end.

		**Two numbers decide the rate**, and both are easy to take from the wrong place:

		- the chip's own rate is `clock / 224` (`multipcm.c:598`), and
		- the voice reads it at `(1024 + pitch) / 1024 * 2^(octave - 1)` (`multipcm.c:671` and the
		  shift at 314-324). **Octave 1 is unity**, so a decoder that reads `2^octave` is an octave
		  high - the mistake the row had before the ROM arrived.

		**Banking arrives as a command, not a register.** VGM's `0xC3` never reaches the chip; libvgm
		turns it into writes to 0x10/0x11/0x12 (`Cmd_YMW_Bank`) and the loader does the same, so the
		walk below sees ordinary register writes. The core then remaps a sample's base through those
		banks (`multipcm.c:733-743`), and so must this. Measured over the corpus: the highest start is
		0x1FF24B in sixteen songs and 0x1F8690 in the seventeenth - **every one of them above the
		1 MB line**, so skipping the remap would read the wrong window for the whole file rather than
		for an edge case.
	*/
	std::vector<playback::DecodedSample> collectMultiPcmSamples (const midi::SongIr& ir, double rateHz, uint32_t clockHz);
}
