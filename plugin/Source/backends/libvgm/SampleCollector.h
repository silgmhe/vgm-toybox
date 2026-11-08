#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "playback/WaveExport.h"

namespace toybox::backends::libvgm
{
	/** How a sample's bytes become levels. */
	enum class SampleFormat
	{
		pcm8Signed,		// one signed byte per level, -128..127
		pcm16Signed,	// two little-endian bytes per level, -32768..32767
		dpcm4,			// four-bit deltas, two per byte, low nibble first, summed and peak-normalised
		kadpcm4,		// the same shape, but its table's eighth entry is -128 rather than 0
		qsoundAdpcm,	// adaptive step size: the QSound's own, nothing like the two above
		signMagnitude8,	// RF5C68/164: bit 7 is a sign, the low seven bits a magnitude
		nesDelta,		// the 2A03's one-bit deltas, expanded by `dpcm::decode`
		msm6295,		// the OKI's four-bit delta codec: a 49-step table, high nibble first
		k007232,		// seven-bit unsigned around 0x40, and the sample ends at the first bit-7 byte
		ga20,			// eight-bit unsigned around 0x80, and the sample ends at the first 0x00 byte
		es5503,		// the same encoding: the DOC halts on 0x00 too (`es5503.c:301`)
		vsu6,			// six-bit unsigned around 0x20: one cycle of thirty-two wave entries
		prom4,			// four-bit unsigned around 0x08: the K005289's PROM, sixteen levels
		pcm8Unsigned,	// eight-bit unsigned around 0x80, length given by the caller
	};

	/** Accumulates one `DecodedSample` per distinct recording.

		The parts every sample collector here does the same way: recognising that a sample has been
		seen already, turning its bytes into levels, and holding the result until the caller takes it.
		What differs - where the bytes live, how a register names them, which format they are in - is
		left with the chip, because those are the parts that are actually different.

		Deliberately not a base class with a virtual `locate()`: the two collectors that exist do not
		share a walk, a memory model or a register layout. A hierarchy would add a vtable and no code.
		What they do share is here, and a third chip gets it by calling `add()`.

		Names come from the caller, not from here. The DPCM files are named after a sixteen-bit
		address and the C140's after a twenty-bit one, and unifying the width would rename every file
		an existing user has. A refactor that changes output is not a refactor (rules, §9).
	*/
	class SampleCollector
	{
	public:
		/** Adds one sample unless `key` was added before.

			`key` is the chip's own way of saying "this recording" - an address, a page and length,
			a folded offset. `name` is the file it will be written as, without the extension.
		*/
		void add (uint64_t key, const std::string& name, const uint8_t* bytes,
				  std::size_t length, SampleFormat format, double rate,
				  uint32_t sampleId = 0);

		/** The samples added so far, in the order they were first seen. */
		std::vector<playback::DecodedSample> take (void);

	private:
		std::map<uint64_t, bool> seen;
		std::vector<playback::DecodedSample> out;
	};
}
