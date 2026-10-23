#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
	An NSF image, read the way the format defines it rather than the way one player happens to.

	NSF is not a register stream like VGM: it is a 6502 program (the driver) plus its data, with a
	header that says where to load it, where to start it, and which extra sound chips the driver
	expects. Playing one means running the program and collecting the writes it makes - which is
	exactly the shape this project already renders, so the format belongs here and not behind a
	converter.

	This struct is only the header and the bytes; running the program is the player's job.
*/

namespace toybox::formats::nsf
{

/** The expansion a module was written for. NSF carries one bit per chip, and its driver expects
	exactly those to be present. */
enum class NsfChip : std::uint8_t
{
	none = 0,
	vrc6,
	vrc7,
	fds,
	mmc5,
	n163,
	fme7
};

struct NsfFile
{
	int version = 0;
	int totalSongs = 0;
	int startSong = 0;			// 1-based, as the header counts

	std::uint16_t loadAddress = 0;
	std::uint16_t initAddress = 0;
	std::uint16_t playAddress = 0;

	std::string title;
	std::string artist;
	std::string copyright;

	/** Time per play call, in microseconds, for each machine. Zero means "use the driver's own". */
	std::uint32_t ntscSpeed = 0;
	std::uint32_t palSpeed = 0;

	/** Which of the eight 8 KB windows the driver switches, or all zero for a plain image. */
	std::uint8_t banks[8] = {};

	NsfChip chip = NsfChip::none;
	/** The header only has one bit per chip, but a driver may need more than one; this is the
		count the bits describe, and 1 for the common case. */
	int chipCount = 0;

	/** The program and its data, starting at `loadAddress`. */
	std::vector<std::uint8_t> rom;
};

/** Reads a header and takes a copy of the program. False (with a reason) for anything malformed. */
bool parseNsf (const void* data, std::size_t size, NsfFile& out, std::string& error);

/** The play rate the file asks for, in frames per second (60 or 50 by default). */
double framesPerSecond (const NsfFile& file);

} // namespace toybox::formats::nsf
