#include "NsfFile.h"

#include <cstring>

namespace toybox::formats::nsf
{

namespace
{

constexpr std::size_t kHeaderSize = 0x80;
constexpr std::size_t kNameLength = 32;

std::uint16_t readWord (const std::uint8_t* bytes)
{
	return (std::uint16_t) (bytes[0] | (bytes[1] << 8));
}

std::string readName (const std::uint8_t* bytes)
{
	// Fixed-width, zero padded, and not necessarily zero terminated when it fills the field.
	std::size_t length = 0;
	while (length < kNameLength && bytes[length] != 0)
	{
		++length;
	}

	return std::string (reinterpret_cast<const char*> (bytes), length);
}

} // namespace

bool parseNsf (const void* data, std::size_t size, NsfFile& out, std::string& error)
{
	const auto* bytes = static_cast<const std::uint8_t*> (data);

	if (bytes == nullptr || size < kHeaderSize)
	{
		error = "the image is smaller than an NSF header";
		return false;
	}

	if (std::memcmp (bytes, "NESM", 4) != 0 || bytes[4] != 0x1A)
	{
		error = "not an NSF image";
		return false;
	}

	out = NsfFile();
	out.version = bytes[5];
	out.totalSongs = bytes[6] > 0 ? bytes[6] : 1;
	out.startSong = bytes[7] > 0 ? bytes[7] : 1;

	// The load address is the one field the format extended: version 1 stores the size of the
	// program instead, and the program always loads at $8000.
	if (out.version == 1)
	{
		out.loadAddress = 0x8000;
	}
	else
	{
		out.loadAddress = readWord (bytes + 0x08);
	}

	out.initAddress = readWord (bytes + 0x0A);
	out.playAddress = readWord (bytes + 0x0C);

	out.title = readName (bytes + 0x0E);
	out.artist = readName (bytes + 0x2E);
	out.copyright = readName (bytes + 0x4E);

	out.ntscSpeed = readWord (bytes + 0x6E);

	for (int i = 0; i < 8; ++i)
	{
		out.banks[i] = bytes[0x70 + i];
	}

	out.palSpeed = readWord (bytes + 0x78);

	const std::uint8_t flags = bytes[0x7A];

	// One bit per chip, in the order the format lists them. A driver that needs two of the same
	// chip sets one bit and says so in its own way; nothing here can express that, so the count
	// stays 1 and the caller treats the bit as "this chip is present".
	if ((flags & 0x01) != 0) { out.chip = NsfChip::vrc6; out.chipCount = 1; }
	if ((flags & 0x02) != 0) { out.chip = NsfChip::vrc7; out.chipCount = 1; }
	if ((flags & 0x04) != 0) { out.chip = NsfChip::fds; out.chipCount = 1; }
	if ((flags & 0x08) != 0) { out.chip = NsfChip::mmc5; out.chipCount = 1; }
	if ((flags & 0x10) != 0) { out.chip = NsfChip::n163; out.chipCount = 1; }
	if ((flags & 0x40) != 0) { out.chip = NsfChip::fme7; out.chipCount = 1; }

	if (out.initAddress == 0)
	{
		error = "the header has no init address";
		return false;
	}

	if (size <= kHeaderSize)
	{
		error = "the image has no program after the header";
		return false;
	}

	out.rom.assign (bytes + kHeaderSize, bytes + size);
	return true;
}

double framesPerSecond (const NsfFile& file)
{
	// The speed is the length of one play call in microseconds, and the format steals its lowest
	// bit as the PAL flag - real speeds are round numbers of microseconds, so the bit is free.
	const bool pal = (file.ntscSpeed & 1u) != 0;
	const std::uint32_t speed = file.ntscSpeed & ~1u;

	if (speed != 0)
	{
		return 1000000.0 / (double) speed;
	}

	// The documented defaults are 16639 us (NTSC) and 19997 us (PAL), not 16666 and 20000: the
	// values come from the frame rate of each machine, not from a rounded 60 or 50.
	return pal ? 1000000.0 / 19997.0 : 1000000.0 / 16639.0;
}

} // namespace toybox::formats::nsf
