#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "formats/nsf/NsfFile.h"

/**
	The address space an NSF driver runs in.

	Two kilobytes of RAM mirrored up to $1FFF, the program at its load address, and writes into the
	sound registers handed to the player instead of being stored. That last part is the whole point:
	in an NSF the music is a program writing to $4000-$4017, so the memory map is where those writes
	are captured.

	Bank switching is supported because real files use it: when the program is larger than the
	thirty-two kilobytes the address space has room for, a write to $5FF8-$5FFF selects which 8 KB
	window of the program the CPU sees from $8000 upwards.
*/

namespace toybox::formats::nsf
{

class NsfMemory
{
public:
	/** Maps the program and installs the interrupt vectors. Call once, before the CPU runs. */
	void load (const NsfFile& file);

	std::uint8_t read (std::uint16_t address) const;
	void write (std::uint16_t address, std::uint8_t value);

	/** Called for every write in $4000-$4017 - the sound registers. Not stored. */
	std::function<void (std::uint16_t, std::uint8_t)> onSoundWrite;

	std::uint16_t initAddress (void) const { return entry; }
	std::uint16_t playAddress (void) const { return play; }

	/** True when the program is bigger than the address space and banks are in use. */
	bool banked (void) const { return rom.size() > 0x8000; }

private:
	/** The byte the program has at a physical offset, allowing for the selected banks. */
	std::uint8_t programByte (std::uint16_t address) const;

	std::vector<std::uint8_t> ram = std::vector<std::uint8_t> (0x800, 0);
	std::vector<std::uint8_t> rom;
	std::uint16_t loadAddress = 0x8000;
	std::uint16_t entry = 0;
	std::uint16_t play = 0;
	std::uint8_t banks[4] = { 0, 1, 2, 3 };		// one per 8 KB window, in address order
};

} // namespace toybox::formats::nsf
