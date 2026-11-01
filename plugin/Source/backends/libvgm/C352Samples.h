#pragma once

#include <vector>

#include "playback/WaveExport.h"

namespace toybox::midi
{
	struct SongIr;
}

namespace toybox::backends::libvgm
{
	/** Every sample the song's C352 voices ever pointed at, decoded once each.

		The chip's registers name a sample with a bank and two sixteen-bit bounds, so walking the
		writes finds all of them without playing the song - the same idea as `NesDpcmBank` and
		`C140Samples`, and the same reason: the export should not have to run the music to know what
		samples it holds.

		Key-on is not a register write. The chip loads a voice's flags with `0x4000` and the driver
		then writes `0x202`, at which point the core sweeps every voice and acts on what it finds. So
		this records the flags as they are written and collects on the execute - anything else would
		miss voices whose key-on bit was set several writes before the sweep.

		mu-law voices are skipped: their bytes are logarithmically coded, so a level per byte would
		be a shape this chip never makes. Noise voices are skipped for a different reason - the bytes
		it "plays" are a shift register, not a recording.

		The rate written is the chip's reference rate, not the voice's own. Every voice plays at its
		own speed, so a file holding many samples has no single "the" rate; the reference is what a
		sampler reading one of them out of context should be handed.
	*/
	std::vector<playback::DecodedSample> collectC352Samples (const midi::SongIr& ir);
}
