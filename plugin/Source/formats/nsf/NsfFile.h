#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
	An NSF image, read from the bytes.

	NSF is not a register dump: it is a 6502 program with a header that says where to put it, where
	to start it and which extra chips the driver expects. Nothing in this class runs anything - it
	only says what the file claims, so a player can be held to it.

	Written from the format's specification rather than ported, and the traps are the reason it is
	worth testing: version 1 stores a size where later versions store a load address, the song count
	and start index are one-based, and the speed field's lowest bit is the PAL flag rather than part
	of the number.
*/
namespace toybox::formats::nsf
{

/** The extra sound hardware a driver wants, as the header's chip byte declares it. */
enum class NsfChip : std::uint8_t
{
	none = 0x00,
	vrc6 = 0x01,
	vrc7 = 0x02,
	fds = 0x04,
	mmc5 = 0x08,
	n163 = 0x10,
	fme7 = 0x20
};

struct NsfFile
{
	/** Size of the header, and where the program starts. */
	static constexpr std::size_t headerSize = 0x80;

	/** Byte offsets the specification fixes. */
	enum Offset : std::size_t
	{
		offsetMagic = 0x00,
		offsetVersion = 0x05,
		offsetTotalSongs = 0x06,
		offsetStartSong = 0x07,
		offsetLoadAddress = 0x08,
		offsetInitAddress = 0x0A,
		offsetPlayAddress = 0x0C,
		offsetName = 0x0E,
		offsetArtist = 0x2E,
		offsetCopyright = 0x4E,
		offsetNtscSpeed = 0x6E,
		offsetBankSwitch = 0x70,
		offsetPalSpeed = 0x78,
		offsetChipFlags = 0x7A,
		offsetExtraChipFlags = 0x7B
	};

	int version = 0;
	int totalSongs = 0;
	int startSong = 1;				// one-based in the file, kept as written
	std::uint16_t loadAddress = 0x8000;
	std::uint16_t initAddress = 0x8000;
	std::uint16_t playAddress = 0x0000;
	std::string name;
	std::string artist;
	std::string copyright;

	/** Play call period in microseconds; the lowest bit of the NTSC word is the PAL flag. */
	std::uint16_t ntscSpeed = 0;
	std::uint16_t palSpeed = 0;
	std::uint8_t chipFlags = 0;			// 0x7A
	std::uint8_t extraChipFlags = 0;	// 0x7B, the NSF 2.0 extension
	std::array<std::uint8_t, 8> bankSwitch {};

	/** The program and its data, from the end of the header to the end of the file. */
	std::vector<std::uint8_t> rom;

	/** FNV-1a over `rom`, computed once when the image is read. It is what the DPCM sample-slot
		cache keys on: a backend can be rebuilt while a song is open, so the file itself is not a
		stable identity, but its bytes are. */
	std::size_t contentHash = 0;

	bool isPal (void) const { return (ntscSpeed & 1u) != 0; }
	/** True when the program cannot be held in the 32 KB the address space gives it. */
	bool isBanked (void) const;
	double framesPerSecond (void) const;
	/** The expansion chip the header declares.

		Two bytes can carry it: 0x7A is where the format puts the flags, and 0x7B is the NSF 2.0
		byte for a second chip. But a lot of files in the wild write only 0x7B and leave 0x7A at
		zero - every VRC6 and VRC7 file in test_file does - so reading one of the two meant those
		songs played as a bare 2A03. Either byte answering is better than that.
	*/
	NsfChip chip (void) const
	{
		if (chipFlags != 0)
		{
			return (NsfChip) chipFlags;
		}

		return (NsfChip) extraChipFlags;
	}

	/** Reads an image. Returns false and fills `error` when the bytes are not a usable NSF. */
	static bool parse (const void* data, std::size_t size, NsfFile& out, std::string& error);

	/** True when `data` starts with the magic this reader accepts. */
	static bool looksLikeNsf (const void* data, std::size_t size);
};

} // namespace toybox::formats::nsf
