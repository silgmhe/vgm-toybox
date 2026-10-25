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

	2. A program larger than the 32 KB the address space holds can only be reached through the
	   eight 4 KB windows the driver selects at $5FF8-$5FFF - one register per window, in address
	   order. Those reads come from the file, not from the copied image, because only one bank of it
	   fits at a time.
*/
namespace toybox::backends::nsf
{

class NsfBus : public xgm::NES_MEM
{
public:
	explicit NsfBus (const formats::nsf::NsfFile& file);

	bool Read (std::uint32_t adr, std::uint32_t& val, std::uint32_t id = 0) override;
	bool Write (std::uint32_t adr, std::uint32_t val, std::uint32_t id = 0) override;

	/** Where sound-register writes go. Set by the player, which owns the chips. */
	std::function<void (std::uint16_t, std::uint8_t)> onSoundWrite;

	/** How many sound-register writes have been forwarded - the cheapest proof the driver ran. */
	std::uint64_t soundWriteCount = 0;

	/** Writes into the $0200 page. A driver that decodes itself into RAM leaves its mark here;
		zero of them means the decoder never ran, which is a different fault from "it ran and did
		nothing". Diagnostics. */
	std::uint64_t ramWriteCount = 0;

	/** Writes to the $0211 gate, and the last value written. Diagnostics: a driver that sets this
		byte has decided its play routine should run, so zero here means init never finished. */
	std::uint64_t gateWrites = 0;
	std::uint8_t gateValue = 0;

	/** Writes seen in each expansion chip's address range, whether or not that chip exists:
		index 0 VRC6, 1 VRC7, 2 N163, 3 FME7. A file may use a chip without declaring it, and
		this is how that is caught before the chip is created. */
	std::array<std::uint32_t, 4> expansionWrites {};

	/** Writes to the addresses that only a real driver of that chip touches, as opposed to any
		write that merely falls inside the range. This is what the detection judges by: \
		\-\ for VRC6 (control and pitch of the first pulse), \/\ for VRC7,
		\/\ for N163, \/\ for FME7. */
	std::array<std::uint32_t, 4> expansionSignature {};

	/** Puts the program in place and installs the six-byte entry trampoline at $4100.
		The order matters: the image is cleared first, the trampoline after. */
	void load (void);

	/** Puts the windows back to the mapping the header declares - what a reset does on hardware. */
	void resetBanks (void);

private:
	const formats::nsf::NsfFile& file;
	bool banked = false;
	std::array<std::uint8_t, 8> banks {};
};

} // namespace toybox::backends::nsf
