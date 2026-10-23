#include "NsfMemory.h"

namespace toybox::formats::nsf
{

void NsfMemory::load (const NsfFile& file)
{
	ram.assign (0x800, 0);
	rom = file.rom;
	loadAddress = file.loadAddress;
	entry = file.initAddress;
	play = file.playAddress;

	// A plain image is one contiguous block from the load address; a banked one starts with the
	// four windows showing the first four banks, which is what a driver expects after reset.
	for (int i = 0; i < 4; ++i)
	{
		banks[i] = (std::uint8_t) i;
	}
}

std::uint8_t NsfMemory::programByte (std::uint16_t address) const
{
	if (! banked())
	{
		// The program covers $8000-$FFFF in the simple case; anything outside it reads as zero.
		const std::uint32_t offset = (std::uint32_t) address - loadAddress;
		return (address >= loadAddress && offset < rom.size()) ? rom[offset] : 0;
	}

	const int window = (address >> 13) & 0x03;		// $8000, $A000, $C000, $E000
	const std::uint32_t offset = ((std::uint32_t) banks[window] << 13) + (address & 0x1FFF);

	return offset < rom.size() ? rom[offset] : 0;
}

std::uint8_t NsfMemory::read (std::uint16_t address) const
{
	if (address < 0x2000)
	{
		return ram[address & 0x07FF];				// two kilobytes, mirrored up
	}

	if (address >= 0x8000)
	{
		return programByte (address);
	}

	// $2000-$5FFF is hardware that an NSF driver rarely touches; $6000-$7FFF is program RAM.
	return 0;
}

void NsfMemory::write (std::uint16_t address, std::uint8_t value)
{
	if (address < 0x2000)
	{
		ram[address & 0x07FF] = value;
		return;
	}

	if (address >= 0x4000 && address <= 0x4017)
	{
		if (onSoundWrite)
		{
			onSoundWrite (address, value);
		}

		return;
	}

	// Banking: the eight registers at $5FF8-$5FFF select the window each 8 KB block shows.
	if (banked() && address >= 0x5FF8 && address <= 0x5FFB)
	{
		// One register per 8 KB window, in address order:  selects ,  selects .
		banks[address - 0x5FF8] = value;
		return;
	}

	// Everything else is ROM or unmapped and absorbs the write. RAM above $2000 does not exist.
}

} // namespace toybox::formats::nsf
