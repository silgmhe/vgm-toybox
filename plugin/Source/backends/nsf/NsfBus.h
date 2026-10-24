#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "formats/nsf/NsfFile.h"
#include "xgm/devices/Memory/nes_mem.h"

/**
	The 6502's address space for an NSF, with two jobs added to the vendored memory map.

	1. Sound registers do not live in memory. A write to $4000-$4017 leaves the map through a
	   callback that feeds the chips; nothing is stored and reading back gives zero, which is what
	   the hardware does and what a driver that polls a register would notice.

	2. A program larger than the 32 KB the address space holds can only be reached through the four
	   8 KB windows the driver selects at $5FF8-$5FFB. Those reads come from the file, not from the
	   copied image, because only one bank of it fits.
*/
namespace toybox::backends::nsf
{

class NsfBus : public xgm::NES_MEM
{
public:
	explicit NsfBus (const formats::nsf::NsfFile& file);

	bool Read (UINT32 adr, UINT32& val, UINT32 id = 0) override;
	bool Write (UINT32 adr, UINT32 val, UINT32 id = 0) override;

	/** Where sound-register writes go. Set by the player, which owns the chips. */
	std::function<void (std::uint16_t, std::uint8_t)> onSoundWrite;

	/** How many sound-register writes have been forwarded - the cheapest proof the driver ran. */
	std::uint64_t soundWriteCount = 0;

	/** Writes seen in each expansion chip's address range, whether or not that chip exists:
		index 0 VRC6, 1 VRC7, 2 N163, 3 FME7. A file may use a chip without declaring it, and
		this is how that is caught before the chip is created. */
	std::array<std::uint32_t, 4> expansionWrites {};

	/** Puts the program in place and installs the six-byte entry trampoline at $4100.
		The order matters: the image is cleared first, the trampoline after. */
	void load (void);

private:
	const formats::nsf::NsfFile& file;
	bool banked = false;
	std::array<std::uint8_t, 4> banks {};
};

} // namespace toybox::backends::nsf
