#pragma once

#include <cstdint>

namespace toybox::chips
{
	/** The ROM offset a C140 voice's bank and address point at.

		libvgm's `find_sample()` (c140.c), which is where the board's wiring lives. The registers carry
		only eight bits of bank, so on System 2 the address is folded into nineteen bits: without that
		fold a one-megabyte ROM has no bank 0x14 and every sample of the song comes out unmeasurable.

		Shared rather than private to the decoder, because two callers need the same answer: the
		decoder, to measure the cycle of the sample a voice is about to play, and the sample
		collector, to cut that sample out of the ROM. Two copies would let the byte a voice plays and
		the byte an export writes come from different addresses - which is the shape of the bug that
		once made a drum sound right and draw as a flat line.
	*/
	inline uint32_t c140SampleOffset (uint32_t address, uint8_t bank, uint8_t banking)
	{
		const uint32_t adrs = ((uint32_t) bank << 16) + address;

		switch (banking)
		{
		case 0x01:		// system21
			return ((adrs & 0x300000u) >> 1) + (adrs & 0x7FFFFu);

		case 0x03:		// linear
			return adrs;

		case 0x00:		// system2, and the default
		default:
			return ((adrs & 0x200000u) >> 2) | (adrs & 0x7FFFFu);
		}
	}
}
