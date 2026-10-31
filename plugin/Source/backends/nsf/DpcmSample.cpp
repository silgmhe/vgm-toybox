#include "DpcmSample.h"

#include <algorithm>

namespace toybox::backends::nsf::dpcm
{

std::size_t lengthFromRegister (uint8_t value)
{
	return (std::size_t) value * 16u + 1u;
}

std::size_t trimFiller (const uint8_t* bytes, std::size_t length)
{
	if (bytes == nullptr)
	{
		return 0;
	}

	while (length > 0)
	{
		const auto byte = bytes[length - 1];

		if (byte != 0x00 && byte != 0x55 && byte != 0xAA && byte != 0xFF)
		{
			break;
		}

		--length;
	}

	return length;
}

std::size_t dropPaddingByte (std::size_t length)
{
	return (length > 0 && (length & 0x0F) == 1) ? length - 1 : length;
}

std::size_t finalLength (std::size_t length, const uint8_t* bytes)
{
	return dropPaddingByte (trimFiller (bytes, length));
}

std::vector<float> decode (const uint8_t* bytes, std::size_t length)
{
	std::vector<float> out;
	out.reserve (length * 8u);

	if (bytes == nullptr)
	{
		return out;
	}

	int level = 0;

	for (std::size_t i = 0; i < length; ++i)
	{
		const auto byte = bytes[i];

		for (int bit = 7; bit >= 0; --bit)
		{
			level += ((byte >> bit) & 1u) != 0 ? 2 : -2;
			level = (std::clamp) (level, -128, 127);

			out.push_back ((float) level / 128.0f);
		}
	}

	return out;
}

double rate (uint8_t index, bool pal)
{
	// Cycles per bit, one entry per rate index. A larger number is a slower sample, which is why
	// the table descends: index 0 is the fastest the chip can read.
	static constexpr int ntsc[16] = { 428, 380, 340, 320, 286, 254, 226, 214,
									  190, 160, 142, 128, 106,  84,  72,  54 };
	static constexpr int palCycles[16] = { 398, 354, 316, 298, 276, 236, 210, 198,
										   176, 148, 132, 118,  98,  78,  66,  50 };

	// The CPU clock the sample is measured against, not the audio rate: DPCM reads one bit per
	// these cycles whatever the mixer is doing.
	static constexpr double kNtscClock = 1789772.7272727273;
	static constexpr double kPalClock = 1662607.0;

	const double clock = pal ? kPalClock : kNtscClock;
	const auto cycles = pal ? palCycles[index & 0x0F] : ntsc[index & 0x0F];

	return clock / (double) cycles;
}

} // namespace toybox::backends::nsf::dpcm
