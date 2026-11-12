#pragma once

#include <vector>

#include "playback/WaveExport.h"

namespace toybox::midi
{
	struct SongIr;
}

namespace toybox::backends::libvgm
{
	/** Every PCM sample the song's X1-010 channels ever started, decoded once each.

		The X1-010 is two players in one chip, and only the PCM half has samples to find: `0x04` is a
		start page and `0x05` an end page, both in 4 KB units of the song's 1 MB `0x91` image. Walking
		the key-on edges therefore finds a sample without playing the song - the same idea as
		`collectC140Samples`, with different registers.

		The rate is the one the voice is playing it at (`clock / 8192` per frequency step). That is a
		real playback rate rather than the C140's reference rate: on this chip the frequency register
		is how fast the bytes are read, so a file written at it plays at the pitch the song used.

		Waveform-mode channels are skipped - they walk on-chip tables, and those are shapes, not
		samples.
	*/
	std::vector<playback::DecodedSample> collectX1010Samples (const midi::SongIr& ir);

	/** **Every wave table the X1-010's waveform channels played**, one file per distinct table.

		The other half of the same chip: thirty-two tables of 128 signed bytes, uploaded into its own
		register space at 0x1000-0x1FFF (not a ROM block), and read at `clock * pitch / 524288`
		entries a second. A channel plays the table its `0x01` register names.

		Named `x1010wave_...` rather than `x1010_...` so a folder holding both halves of one song
		does not mix a drum with an oscillator.
	*/
	std::vector<playback::DecodedSample> collectX1010WaveSamples (const midi::SongIr& ir,
																 uint32_t clockHz);
}
