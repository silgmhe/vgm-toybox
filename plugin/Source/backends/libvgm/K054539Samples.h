#pragma once

#include <vector>

#include "playback/WaveExport.h"

namespace toybox::midi
{
	struct SongIr;
}

namespace toybox::backends::libvgm
{
	/** Every PCM sample a K054539 song plays, in the order the drivers first started them.

		The fourth way these chips name a sample, and the strangest: no length and no end address, a
		*marker* instead - the core stops a voice when the unit it just fetched is `0x80` (eight-bit),
		`0x8000` (sixteen-bit) or the byte `0x88` (four-bit DPCM). The distance to that marker is the
		sample, found by scanning, which is exact because the ROM is here.

		Two formats are exported and one is not. Eight-bit and sixteen-bit are levels and have a
		`SampleFormat` each; four-bit DPCM is deltas and would need a decoder, so those voices are
		skipped - the same line the K053260 draws for KADPCM.
	*/
	std::vector<playback::DecodedSample> collectK054539Samples (const midi::SongIr& ir);
}
