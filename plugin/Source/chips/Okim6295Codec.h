#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

/**
 *	The MSM6295's 4-bit delta codec, in one place.

	Two readers need it and they must agree: the decoder, which measures the sample's cycle to give
	the channel a note, and the sample collector, which turns the ROM bytes into levels for an
	exported file. Written twice, the two would drift - and they would drift silently, because each
	produces a plausible waveform on its own. The tables are the hardware's (`okim6295.c`), and the
	order is the chip's: the **high** nibble of a byte comes first, unlike both Konami formats.

	Kept as a header with a template rather than a translation unit with a callback: the fetch is a
	byte out of a `std::vector<uint8_t>`, and an indirect call per nibble is the only thing a shared
	object file would buy.
*/
namespace toybox::chips::okim
{
	inline constexpr int kStepTable[49] = {
		16,   17,   19,   21,   23,   25,   28,   31,   34,   37,   41,   45,   50,   55,   60,
		66,   73,   80,   88,   97,   107,  118,  130,  143,  157,  173,  190,  209,  230,  253,
		279,  307,  337,  371,  408,  449,  494,  544,  598,  658,  724,  796,  876,  963,  1060,
		1166, 1282, 1411, 1552 };

	inline constexpr int kIndexShift[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

	/** The codec's own range: the accumulator is clamped to twelve signed bits. */
	inline constexpr double kFullScale = 2048.0;

	/** Decodes `nibbles` levels starting at `startNibble`.

		`byteAt` takes a byte address. Nibble addresses are the chip's own unit - the ROM's sample
		table stores start and stop that way - so the division by two happens here, once.
	*/
	template <typename ByteAt>
	std::vector<double> decode (ByteAt&& byteAt, uint32_t startNibble, uint32_t nibbles)
	{
		std::vector<double> waveform;
		waveform.reserve (nibbles);

		int signal = 0;
		int stepIndex = 0;

		for (uint32_t i = 0; i < nibbles; ++i)
		{
			const uint8_t byte = byteAt (startNibble + i / 2);
			const int nibble = (i % 2 == 0) ? (byte >> 4) : (byte & 0x0F);

			const int magnitude = nibble & 0x07;
			const int delta = kStepTable[stepIndex] * magnitude / 2;

			signal += (nibble & 0x08) != 0 ? -delta : delta;
			signal = std::max (-2048, std::min (2047, signal));

			stepIndex += kIndexShift[nibble];
			stepIndex = std::max (0, std::min (48, stepIndex));

			waveform.push_back ((double) signal);
		}

		return waveform;
	}
}
