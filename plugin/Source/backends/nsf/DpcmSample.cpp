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

} // namespace toybox::backends::nsf::dpcm
