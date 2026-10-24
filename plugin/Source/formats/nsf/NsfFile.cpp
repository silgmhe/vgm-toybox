#include "formats/nsf/NsfFile.h"

#include <cstring>

namespace toybox::formats::nsf
{

namespace
{

std::uint16_t readLE16 (const std::uint8_t* p)
{
	return (std::uint16_t) ((std::uint16_t) p[0] | ((std::uint16_t) p[1] << 8));
}

/** The name fields are fixed-length and not necessarily terminated. Trailing spaces are part of the
	padding, not of the title, and control bytes are dropped rather than shown as boxes. */
std::string readField (const std::uint8_t* p, std::size_t length)
{
	std::string out;
	out.reserve (length);

	for (std::size_t i = 0; i < length; ++i)
	{
		const auto c = (char) p[i];

		if (c == '\0')
		{
			break;
		}

		const auto byte = (unsigned char) c;

		if (byte < 0x20 || byte == 0x7F)
		{
			continue;
		}

		out.push_back (c);
	}

	while (! out.empty() && out.back() == ' ')
	{
		out.pop_back();
	}

	// A field that is only padding should read as empty, not as a run of spaces.
	const auto first = out.find_first_not_of (' ');

	return first == std::string::npos ? std::string() : out.substr (first);
}

} // namespace

bool NsfFile::looksLikeNsf (const void* data, std::size_t size)
{
	if (data == nullptr || size < headerSize)
	{
		return false;
	}

	return std::memcmp (data, "NESM", 4) == 0;
}

bool NsfFile::isBanked (void) const
{
	// The address space holds 32 KB of program. Anything larger can only be reached by switching
	// the windows the driver writes to $5FF8-$5FFB.
	return rom.size() > 0x8000;
}

double NsfFile::framesPerSecond (void) const
{
	// The speed is the length of one play call in microseconds, and the format steals its lowest
	// bit as the PAL flag - real speeds are round numbers of microseconds, so the bit is free.
	const std::uint32_t speed = (std::uint32_t) (ntscSpeed & ~1u);

	if (speed != 0)
	{
		return 1000000.0 / (double) speed;
	}

	// The documented defaults come from each machine's frame rate, which is why they are 16639 and
	// 19997 microseconds rather than a rounded 16666 and 20000.
	return isPal() ? 1000000.0 / 19997.0 : 1000000.0 / 16639.0;
}

bool NsfFile::parse (const void* data, std::size_t size, NsfFile& out, std::string& error)
{
	if (data == nullptr || size < headerSize)
	{
		error = "the file is shorter than an NSF header";
		return false;
	}

	const auto* bytes = static_cast<const std::uint8_t*> (data);

	if (std::memcmp (bytes, "NESM", 4) != 0)
	{
		error = "the file does not start with NESM";
		return false;
	}

	NsfFile file;

	file.version = bytes[offsetVersion];
	file.totalSongs = bytes[offsetTotalSongs] != 0 ? bytes[offsetTotalSongs] : 1;
	file.startSong = bytes[offsetStartSong] != 0 ? bytes[offsetStartSong] : 1;

	// Version 1 stores the program's size in the load field and always loads at $8000; later
	// versions store the load address itself.
	if (file.version == 1)
	{
		file.loadAddress = 0x8000;
	}
	else
	{
		file.loadAddress = readLE16 (bytes + offsetLoadAddress);
	}

	file.initAddress = readLE16 (bytes + offsetInitAddress);
	file.playAddress = readLE16 (bytes + offsetPlayAddress);

	file.name = readField (bytes + offsetName, 32);
	file.artist = readField (bytes + offsetArtist, 32);
	file.copyright = readField (bytes + offsetCopyright, 32);

	file.ntscSpeed = readLE16 (bytes + offsetNtscSpeed);
	file.palSpeed = readLE16 (bytes + offsetPalSpeed);
	file.chipFlags = bytes[offsetChipFlags];

	for (std::size_t i = 0; i < file.bankSwitch.size(); ++i)
	{
		file.bankSwitch[i] = bytes[offsetBankSwitch + i];
	}

	file.rom.assign (bytes + headerSize, bytes + size);

	if (file.rom.empty())
	{
		error = "the header is followed by no program";
		return false;
	}

	out = std::move (file);
	return true;
}

} // namespace toybox::formats::nsf
