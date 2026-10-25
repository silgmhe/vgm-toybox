#if defined(_WIN32)
#include <windows.h>
#else
#include <iconv.h>
#endif

#include "formats/nsf/NsfFile.h"

#include <cstring>

namespace toybox::formats::nsf
{

namespace
{

uint16_t readLE16 (const uint8_t* p)
{
	return (uint16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8));
}

/** The name fields are fixed-length and not necessarily terminated. Trailing spaces are part of the
	padding, not of the title, and control bytes are dropped rather than shown as boxes. */
namespace
{

/** Text fields are ASCII in most files and Shift-JIS in Japanese ones. CP932 is a superset of
	ASCII, so a plain-ASCII field is returned byte for byte; anything else is decoded so the tag
	shows the title instead of boxes. A field that is already valid UTF-8 is left alone, and a field
	that will not decode is returned as it came - showing something is better than dropping it. */
std::string decodeTextField (const std::string& raw)
{
	bool ascii = true;

	for (unsigned char c : raw)
	{
		if (c >= 0x80) { ascii = false; break; }
	}

	if (ascii || raw.empty ())
	{
		return raw;
	}

	// Already UTF-8? Then it is not Shift-JIS and must not be converted twice.
	{
		bool utf8 = true;
		std::size_t i = 0;

		while (i < raw.size () && utf8)
		{
			const auto lead = (unsigned char) raw[i];

			if (lead < 0x80) { ++i; continue; }

			const int extra = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : -1;

			if (extra < 0 || i + (std::size_t) extra >= raw.size ()) { utf8 = false; break; }

			for (int k = 1; k <= extra; ++k)
			{
				if (((unsigned char) raw[i + (std::size_t) k] & 0xC0) != 0x80) { utf8 = false; break; }
			}

			i += (std::size_t) extra + 1;
		}

		if (utf8) { return raw; }
	}

#if defined(_WIN32)
	{
		const int wide = MultiByteToWideChar (932, 0, raw.c_str (), (int) raw.size (), nullptr, 0);


		if (wide > 0)
		{
			std::wstring buffer ((std::size_t) wide, L'\0');

			if (MultiByteToWideChar (932, 0, raw.c_str (), (int) raw.size (), &buffer[0], wide) == wide)
			{
				const int utf8 = WideCharToMultiByte (CP_UTF8, 0, buffer.c_str (), wide, nullptr, 0,
													  nullptr, nullptr);

				if (utf8 > 0)
				{
					std::string out ((std::size_t) utf8, '\0');
					WideCharToMultiByte (CP_UTF8, 0, buffer.c_str (), wide, &out[0], utf8, nullptr, nullptr);
					return out;
				}
			}
		}
	}
#else
	// iconv, because the same title has to read on every platform this builds for. macOS spells the
	// encoding Shift_JIS and Linux accepts both that and CP932; neither has MultiByteToWideChar, so
	// before this the Japanese branch fell through to `return raw` and the card showed the raw
	// Shift-JIS bytes as mojibake. The test caught it on macOS only because Linux had never run it.
	iconv_t conversion = iconv_open ("UTF-8", "SHIFT_JIS");

	if (conversion != (iconv_t) -1)
	{
		// Three UTF-8 bytes per Shift-JIS pair at worst, plus room for the terminator iconv does
		// not write.
		std::string out (raw.size () * 3 / 2 + 4, '\0');
		char* in = const_cast<char*> (raw.data ());
		std::size_t inLeft = raw.size ();
		char* outPtr = &out[0];
		std::size_t outLeft = out.size ();

		const bool ok = iconv (conversion, &in, &inLeft, &outPtr, &outLeft) != (std::size_t) -1;
		iconv_close (conversion);

		if (ok)
		{
			out.resize (out.size () - outLeft);
			return out;
		}
	}
#endif

	return raw;
}

} // namespace

std::string readField (const uint8_t* p, std::size_t length)
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
	const uint32_t speed = (uint32_t) (ntscSpeed & ~1u);

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

	const auto* bytes = static_cast<const uint8_t*> (data);

	if (std::memcmp (bytes, "NESM", 4) != 0)
	{
		error = "the file does not start with NESM";
		return false;
	}

	NsfFile file;

	file.version = bytes[offsetVersion];
	file.totalSongs = bytes[offsetTotalSongs] != 0 ? bytes[offsetTotalSongs] : 1;
	file.startSong = bytes[offsetStartSong] != 0 ? bytes[offsetStartSong] : 1;

	// The load address is the load address in every version: the field holds it, and zero is the
	// documented shorthand for $8000. Treating version 1 as if it stored a size instead - and
	// hard-coding $8000 - relocates every file that declares anything else, which is how a driver
	// at $8AFF ended up executing the bytes that live at $8000.
	const uint16_t declaredLoad = readLE16 (bytes + offsetLoadAddress);
	file.loadAddress = declaredLoad != 0 ? declaredLoad : 0x8000;

	file.initAddress = readLE16 (bytes + offsetInitAddress);
	file.playAddress = readLE16 (bytes + offsetPlayAddress);

	file.name = decodeTextField (readField (bytes + offsetName, 32));
	file.artist = decodeTextField (readField (bytes + offsetArtist, 32));
	file.copyright = decodeTextField (readField (bytes + offsetCopyright, 32));

	file.ntscSpeed = readLE16 (bytes + offsetNtscSpeed);
	file.palSpeed = readLE16 (bytes + offsetPalSpeed);
	file.chipFlags = bytes[offsetChipFlags];
	file.extraChipFlags = bytes[offsetExtraChipFlags];

	for (std::size_t i = 0; i < file.bankSwitch.size(); ++i)
	{
		file.bankSwitch[i] = bytes[offsetBankSwitch + i];
	}

	file.rom.assign (bytes + headerSize, bytes + size);

	// FNV-1a over the program. The sample-slot cache needs an identity for the song that survives
	// a backend being rebuilt, and its bytes are the only one that does.
	std::size_t hash = (std::size_t) 1469598103934665603ull;

	for (uint8_t byte : file.rom)
	{
		hash ^= (std::size_t) byte;
		hash *= (std::size_t) 1099511628211ull;
	}

	file.contentHash = hash;

	if (file.rom.empty())
	{
		error = "the header is followed by no program";
		return false;
	}

	out = std::move (file);
	return true;
}

} // namespace toybox::formats::nsf
