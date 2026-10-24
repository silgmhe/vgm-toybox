#include "backends/nsf/NsfBus.h"

namespace toybox::backends::nsf
{

namespace
{

/**
	The program the CPU jumps to instead of the driver.

	run_from() points the PC at $4100 and patches the two operand bytes of the JSR there with the
	address it wants to call, then treats $4103 as a breakpoint: reaching the JMP-to-self means the
	driver's init or play returned. The rest of the program is NSFPlay's own - it saves A/X/Y and
	masks the NMI so an INIT that never returns still leaves the machine usable.
*/
const UINT8 kPlayerProgram[] =
{
	0x20, 0x1E, 0x41,				// $4100 JSR $411E   (operand patched by run_from)
	0x4C, 0x03, 0x41,				// $4103 JMP $4103   (the breakpoint)
	0x48, 0x8A, 0x48, 0x98, 0x48,	// $4106 PHA TXA PHA TYA PHA
	0xA9, 0x00, 0x8D, 0x00, 0x20,	// $410B LDA #$00 STA $2000 (no NMI re-entry)
	0x20, 0x1E, 0x41,				// $4110 JSR $411E   (play, for the non-returning case)
	0xA9, 0x80, 0x8D, 0x00, 0x20,	// $4113 LDA #$80 STA $2000 (re-enable)
	0x68, 0xA8, 0x68, 0xAA, 0x68,	// $4118 PLA TAY PLA TAX PLA
	0x40,							// $411D RTI
	0x60							// $411E RTS
};

} // namespace

NsfBus::NsfBus (const formats::nsf::NsfFile& image) : file (image)
{
	banked = file.isBanked ();
	resetBanks ();
}

void NsfBus::resetBanks (void)
{
	// With no mapping declared, the first eight banks are visible - the state a driver expects
	// after reset. A banked song that is restarted (a seek, a loop) has to come back to this
	// mapping: the driver left its own selection behind, and starting in the middle of it runs
	// the wrong code, which sounds like one chip having gone quiet.
	bool declared = false;

	for (std::size_t i = 0; i < banks.size (); ++i)
	{
		declared = declared || (i < file.bankSwitch.size () && file.bankSwitch[i] != 0);
	}

	for (std::size_t i = 0; i < banks.size (); ++i)
	{
		banks[i] = declared && i < file.bankSwitch.size () ? file.bankSwitch[i] : (std::uint8_t) i;
	}
}

void NsfBus::load (void)
{
	// SetImage clears the whole 64 KB before copying, so anything written before it is lost - the
	// trampoline in particular. It goes in afterwards, every time.
	resetBanks ();
	SetImage (const_cast<UINT8*> (file.rom.data()), file.loadAddress, (UINT32) file.rom.size());
	SetReserved (kPlayerProgram, (UINT32) sizeof (kPlayerProgram));
}

bool NsfBus::Read (UINT32 adr, UINT32& val, UINT32 id)
{
	if (banked && adr >= 0x8000 && adr <= 0xFFFF)
	{
		// Eight 4 KB windows, selected one register each: $5FF8 is the one at $8000 and $5FFF the
		// one at $F000. Bank numbers are counted in the same 4 KB units.
		const std::uint32_t window = (adr >> 12) & 0x07u;
		// The windows are counted from the load address, which is not required to be 4 KB aligned:
// a file loaded at \ puts its first byte at window 0 offset \, not at offset 0.
		const std::uint32_t bankBase = (std::uint32_t) (file.loadAddress & 0x0FFF);
		const std::uint32_t offset = (std::uint32_t) banks[window] * 0x1000u + (adr & 0x0FFFu) - bankBase;

		val = offset < file.rom.size() ? file.rom[offset] : 0;
		return true;
	}

	return xgm::NES_MEM::Read (adr, val, id);
}

bool NsfBus::Write (UINT32 adr, UINT32 val, UINT32 id)
{

	if (adr == 0x0211)
	{
		++gateWrites;
		gateValue = (std::uint8_t) (val & 0xFF);
	}

	if (adr >= 0x0200 && adr <= 0x02FF)
	{
		++ramWriteCount;
	}
	if (banked && adr >= 0x5FF8 && adr <= 0x5FFF)
	{
		banks[adr - 0x5FF8] = (std::uint8_t) (val & 0xFF);

		// Stored as well as applied: a driver is allowed to read the register back to remember what
		// it selected, and answering zero there is a silent difference from hardware.
		xgm::NES_MEM::Write (adr, val, id);
		return true;
	}

	// Counted before dispatch, and counted even when the chip was never created: that is exactly
	// the case worth catching - the driver writing to a chip its header did not mention.
	bool expansion = true;

	if (adr >= 0x9000 && adr <= 0xB003)
	{
		++expansionWrites[0];
		if (adr <= 0x9003) { ++expansionSignature[0]; }
	}
	else if (adr == 0x9010 || adr == 0x9030)
	{
		++expansionWrites[1];
		++expansionSignature[1];
	}
	else if (adr == 0x4800 || adr == 0xF800)
	{
		++expansionWrites[2];
		++expansionSignature[2];
	}
	else if (adr == 0xC000 || adr == 0xE000)
	{
		++expansionWrites[3];
		++expansionSignature[3];
	}
	else { expansion = false; }

	if (expansion || (adr >= 0x4000 && adr <= 0x4017))
	{
		// The registers are the music, whichever chip owns them. They leave the map here rather
		// than being stored - and an expansion write has to leave it too, or the chip is created,
		// ticked, mixed and never told anything.
		if (onSoundWrite)
		{
			onSoundWrite ((std::uint16_t) adr, (std::uint8_t) (val & 0xFF));
		}

		++soundWriteCount;
		return true;
	}

	return xgm::NES_MEM::Write (adr, val, id);
}

} // namespace toybox::backends::nsf
