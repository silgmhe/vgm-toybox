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

	bool Read (uint32_t adr, uint32_t& val, uint32_t id = 0) override;
	bool Write (uint32_t adr, uint32_t val, uint32_t id = 0) override;

	/** Where sound-register writes go. Set by the player, which owns the chips. */
	std::function<void (uint16_t, uint8_t)> onSoundWrite;

	/** How many sound-register writes have been forwarded - the cheapest proof the driver ran. */
	uint64_t soundWriteCount = 0;

	/** Writes into the $0200 page. A driver that decodes itself into RAM leaves its mark here;
		zero of them means the decoder never ran, which is a different fault from "it ran and did
		nothing". Diagnostics. */
	uint64_t ramWriteCount = 0;

	/** Writes to the $0211 gate, and the last value written. Diagnostics: a driver that sets this
		byte has decided its play routine should run, so zero here means init never finished. */
	uint64_t gateWrites = 0;
	uint8_t gateValue = 0;

	/** Writes seen in each expansion chip's address range, whether or not that chip exists:
		index 0 VRC6, 1 VRC7, 2 N163, 3 FME7. A file may use a chip without declaring it, and
		this is how that is caught before the chip is created. */
	std::array<uint32_t, 4> expansionWrites {};

	/** Writes to the addresses that only a real driver of that chip touches, as opposed to any
		write that merely falls inside the range. This is what the detection judges by: \
		\-\ for VRC6 (control and pitch of the first pulse), \/\ for VRC7,
		\/\ for N163, \/\ for FME7. */
	std::array<uint32_t, 4> expansionSignature {};

	/** One byte of the address space, for readers that only look.

		Read() is the CPU's own access and takes a non-const bus; the display must not perturb the
		bank mapping or the write counters, and both are only touched by Write(), so the distinction
		here is about intent rather than about what happens to the object.
	*/
	uint8_t peek (uint16_t address) const;

	/** The DPCM sample the last start selected.

		Latched at the write that starts it rather than read off the registers afterwards: a driver
		is free to write the next sample's address and length while the current one is still playing,
		and the register would then describe a sample nobody is hearing.
	*/
	struct DpcmSelection
	{
		uint16_t start = 0;		// absolute CPU address: $C000 + page * 64
		uint16_t length = 0;	// bytes: (register & 0x0F) * 16 + 1
		uint8_t rateIndex = 0;	// $4010 bits 0..3, which pick the read rate
		bool loop = false;		// $4010 bit 6
		bool valid = false;

		/** The bytes as they stood at the moment the sample started.

			Copied here rather than read when someone asks for them, because the window $C000 lives
			in is a bank the driver owns: it can point that window at something else the instant the
			hit is over, and a later read then describes whatever the driver put there instead. What
			that reads as is noise with no envelope - the shape of code, not of a drum.

			193 is the longest length the register can name.
		*/
		std::array<uint8_t, 193> bytes {};
	};

	DpcmSelection dpcm;

	/** How many times the enable bit has started a sample. Diagnostics: a hit that sounds longer
		than one sample can be described by a driver that retriggers it, and the count is what tells
		that apart from a sample that really is longer. */
	uint32_t dpcmStarts = 0;

	/** Writes to $4011, the DMC's DAC, and the value it holds.

		This is how a NES drum gets its decay: the sample data is a stream of fixed-size steps with
		no envelope of its own, and the volume comes from the driver walking the DAC down while the
		sample plays. A sample exported without it is the drum without its shape.
	*/
	uint32_t dacWrites = 0;
	uint8_t dacValue = 0;

	/** Puts the program in place and installs the six-byte entry trampoline at $4100.
		The order matters: the image is cleared first, the trampoline after. */
	void load (void);

	/** Puts the windows back to the mapping the header declares - what a reset does on hardware. */
	void resetBanks (void);

private:
	const formats::nsf::NsfFile& file;
	bool banked = false;
	std::array<uint8_t, 8> banks {};

	/** What the last $4012 and $4013 wrote, waiting for the $4015 write that starts the sample.

		Two separate registers and one event: a driver writes the page, then the length, then flips
		the enable bit, and only at that third write does the pair describe something that plays.
	*/
	uint8_t pendingDpcmPage = 0;
	uint8_t pendingDpcmLength = 0;
	uint8_t pendingDpcmRate = 0;
	bool pendingDpcmLoop = false;

	/** Whether the last $4015 write left the DPCM enable set, so the next one can tell a start from
		a write that merely mentions it.

		Only the 0 -> 1 edge begins a sample. A driver that keeps the bit set and writes $4015 for
		the pulse channels every frame would otherwise latch on every one of those writes, and the
		pair it latched would be whatever the registers happened to hold at that moment.
	*/
	bool dpcmEnabled = false;
};

} // namespace toybox::backends::nsf
