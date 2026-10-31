#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

/** The DMC's sample format, as arithmetic that can be called.

	These three steps used to live in NsfPlayer's anonymous namespace, where nothing outside could
	reach them and nothing could assert them. Every error this feature has had so far was in one of
	them - a length read four bits wide, a tail that kept its filler, a padding byte counted as
	data - and each one was found by comparing an exported file against the one FamiStudio produced,
	which is a slow way to learn that a number is off by one.

	The bounds come from the register rather than from any song. $4013 counts sixteen-byte units
	across eight bits and adds one, so 4081 bytes is the most it can name; the samples in the test
	files are 176, 320, 512 and 688, and none of those is the bound.
*/
namespace toybox::backends::nsf::dpcm
{

/** The longest sample the length register can name: (0xFF * 16) + 1. */
constexpr std::size_t kMaxBytes = 4081;

/** One sample the chip read, kept with the bytes it read at that moment.

	A copy rather than an address, because the bank window belongs to the driver: the same address
	read later describes whatever it points at by then, and the address, the length and the rate
	would all still match - which is what makes that failure mode silent.

	The window is a whole bank rather than `length`, because the register's length may be longer
	than the sample and the tail is trimmed later.
*/
struct CapturedSample
{
	std::uint16_t start = 0;
	std::uint16_t length = 0;		// as the register names it; trim before use
	std::uint8_t rateIndex = 0;
	std::array<std::uint8_t, 4096> bytes {};
};

/** What a written `$4013` asks for, in bytes. */
std::size_t lengthFromRegister (uint8_t value);

/** `length` with the filler that follows the data removed.

	0x55 and 0xAA are silence in DPCM - every bit alternates, so the level walks nowhere - and
	0x00 and 0xFF are what an unwritten region holds. A sample ends where they begin.
*/
std::size_t trimFiller (const uint8_t* bytes, std::size_t length);

/** `length` with the padding byte removed, when it ends one.

	The length formula adds a byte the hardware appends to every sample - 0xF0 in these images,
	0x00 in others. It reads as data to any test that looks at bytes, and only the register's own
	arithmetic shows it was never asked for: a length that lands on 16k+1 carries it.
*/
std::size_t dropPaddingByte (std::size_t length);

/** Both of the above, in the order a sample needs them: filler first, then the padding byte. */
std::size_t finalLength (std::size_t length, const uint8_t* bytes);

/** `bytes` integrated into -1..1, eight one-bit steps per byte.

	A DPCM byte is not a sample, it is eight steps: each bit means "up two" or "down two" and the
	level is their running sum. The level is the chip's seven-bit DAC, so it is clamped to -128..127
	and scaled by 128 rather than by 127 - the range is asymmetrical and a symmetric divisor would
	flatten the positive rail.
*/
std::vector<float> decode (const uint8_t* bytes, std::size_t length);

} // namespace toybox::backends::nsf::dpcm
