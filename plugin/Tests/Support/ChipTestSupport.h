#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "chips/ChipTypes.h"
#include "Support/TestSupport.h"

namespace toybox::test
{

using toybox::chips::NoteEvent;
using toybox::chips::RegisterWrite;

/** The note the chip actually sounds: the transposition layer plus the sample's own root.

	Before the two layers existed the decoder reported this sum directly, so the older expectations
	("64 bytes a cycle at 28160 a second is A4") still hold - as a statement about the absolute pitch
	- while `note` alone is the register-only answer. */
inline int absoluteNote (const NoteEvent& note)
{
	return (int) std::lround ((double) (note.note * 100 + note.rootCents) / 100.0);
}

/** Keeps the notes easy to read: `stream.at (44100, 0x03, 0x00)` is a write at one second. */
struct Stream
{
	std::vector<RegisterWrite> writes;

	void at (std::uint32_t time, std::uint8_t reg, std::uint8_t value)
	{
		RegisterWrite write;
		write.reg = reg;
		write.value = value;
		write.sampleTime = time;
		writes.push_back (write);
	}

	void at (std::uint32_t time, std::uint8_t port, std::uint8_t reg, std::uint8_t value)
	{
		RegisterWrite write;
		write.port = port;
		write.reg = reg;
		write.value = value;
		write.sampleTime = time;
		writes.push_back (write);
	}
};

} // namespace toybox::test
