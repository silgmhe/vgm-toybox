#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace toybox::chips::ymz280b
{
	/** `ymz280b.c:205` - the step-size index shift table. */
	constexpr int kIndexScale[8] = { 0x0e6, 0x0e6, 0x0e6, 0x0e6, 0x133, 0x199, 0x200, 0x266 };

	/** `ymz280b.c:316-320`: `value = (nibble & 7) * 2 + 1`, sign from bit 3. */
	inline int diffOf (int nibble)
	{
		const int value = (nibble & 0x07) * 2 + 1;

		return ((nibble & 0x08) != 0) ? -value : value;
	}

	/** **One run of the sample ROM, decoded the way the chip plays it.**

		The mode comes from the control register (bits 5-6): 1 is four-bit Yamaha ADPCM, 2 eight-bit
		PCM, 3 sixteen-bit PCM (`ymz280b.c:655-657`). Positions are in nibbles for ADPCM and in
		samples for the PCM modes - the core keeps one counter and steps it by 2 or 4 per sample
		(`ymz280b.c:466`, `537`), which is the same thing.

		Shared by the row and the collector so a drawn window and an exported file are decoded by the
		same arithmetic - the failure this prevents is a row that looks right while the file does not.
	*/
	inline void decodeRange (const std::vector<uint8_t>& rom, int mode, uint32_t start,
							  uint32_t steps, std::vector<float>& out)
	{
		out.clear ();

		if (rom.empty () || steps == 0)
		{
			return;
		}

		out.reserve ((std::size_t) steps);

		if (mode == 1)
		{
			// **The state is zero at the run's own start, which is exact here**: playbeg begins a
			// voice's stream and the chip resets `signal` and `step` on key-on (`ymz280b.c:846-848`).
			int32_t signal = 0;
			int32_t step = 0x7f;

			for (uint32_t i = 0; i < steps; ++i)
			{
				const uint32_t position = start + i;
				const std::size_t byte = (std::size_t) ((position / 2) % (uint32_t) rom.size ());
				const int nibble = (rom[byte] >> ((~position & 1u) << 2)) & 0x0F;

				signal = (signal * 254) / 256 + (step * diffOf (nibble)) / 8;
				signal = std::clamp (signal, -32768, 32767);

				step = (step * kIndexScale[nibble & 0x07]) >> 8;
				step = std::clamp (step, 0x7f, 0x6000);

				out.push_back ((float) signal / 32768.0f);
			}

			return;
		}

		const int width = (mode == 3) ? 2 : 1;

		for (uint32_t i = 0; i < steps; ++i)
		{
			std::size_t byte = (std::size_t) (((start + i) * (uint32_t) width) % rom.size ());
			int value = 0;

			for (int b = 0; b < width; ++b, byte = (byte + 1) % rom.size ())
			{
				value |= (int) rom[byte] << (8 * b);
			}

			if (width == 2)
			{
				out.push_back ((float) (int16_t) (uint16_t) value / 32768.0f);
			}
			else
			{
				out.push_back (((float) (int8_t) (uint8_t) value) / 128.0f);
			}
		}
	}

	/** The rate a voice steps at: `clock / 384 * (fnum + 1) / 256`.

		`0x172` steps of fnum span 0.172 to 44.1 kHz in 256 steps (`ymz280b.c:18`), so 16 934 400 /
		384 is exactly the 44.1 kHz top of the range. The ADPCM mode reads only eight bits of fnum
		(`ymz280b.c:259-262`), the PCM modes all nine.
	*/
	inline double rateOf (uint32_t clock, int fnum, int mode)
	{
		const double steps = (double) ((mode == 1) ? (fnum & 0xFF) : (fnum & 0x1FF)) + 1.0;

		return ((double) clock / 384.0) * steps / 256.0;
	}
}
