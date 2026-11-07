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
	/** Every table the song's ES5503 oscillators ever started.

		The Ensoniq DOC is a **wavetable** chip, not a sample player with a length register: an
		oscillator walks forward from its table pointer and the core halts it on the first **0x00**
		byte (`es5503.c:301`) - the same marker the GA20 uses, with the same eight-bit unsigned data
		(`es5503.c:310`: `(data - 0x80) * vol`). Everything the export needs is therefore in RAM
		already; the registers only say *which* bytes and *how fast*.

		**The rate is the interesting part.** `es5503_get_sample_rate` is
		`(clock / 8) / (oscsenabled + 2)`, so the chip's own sample rate **moves with the number of
		enabled oscillators** (register 0xE1). One oscillator reads `freq / 2^(9 + resolution - size)`
		bytes per chip sample, so a voice's rate is that step times a rate that other voices can
		change. That is why the walk below keeps `0xE1` at all: it is not a detail of chip setup, it
		is the other half of every playing voice's speed.

		**The RAM is a load-time image.** These three songs upload every table before the first
		register write (measured: the first 0x67 block sits at byte 224, the first 0xD5 at 5388), so
		`memory::ram` builds the whole picture and no replay is needed. A song that streams tables
		mid-playback would need the RF5C164 treatment; the decoder's `attachMemory` cannot express
		it either, so the limitation is the same in both places rather than a new one here.
	*/
	std::vector<playback::DecodedSample> collectEs5503Samples (const midi::SongIr& ir, uint32_t clockHz);
}
