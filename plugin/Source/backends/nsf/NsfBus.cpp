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
	banked = file.isBanked();

	// A banked program starts with the first four banks in the windows, which is the state a
	// driver expects after reset. Files that declare their own selection are honoured.
	bool declared = false;

	for (int i = 0; i < 4; ++i)
	{
		declared = declared || file.bankSwitch[(std::size_t) i] != 0;
	}

	for (int i = 0; i < 4; ++i)
	{
		banks[(std::size_t) i] = declared ? file.bankSwitch[(std::size_t) i] : (std::uint8_t) i;
	}
}

void NsfBus::load (void)
{
	// SetImage clears the whole 64 KB before copying, so anything written before it is lost - the
	// trampoline in particular. It goes in afterwards, every time.
	SetImage (const_cast<UINT8*> (file.rom.data()), file.loadAddress, (UINT32) file.rom.size());
	SetReserved (kPlayerProgram, (UINT32) sizeof (kPlayerProgram));
}

bool NsfBus::Read (UINT32 adr, UINT32& val, UINT32 id)
{
	if (banked && adr >= 0x8000 && adr <= 0xFFFF)
	{
		const std::uint32_t window = (adr >> 13) & 0x03u;

		// Bank numbers count 4 KB units, so an 8 KB window starts that many kilobytes into the file.
		const std::uint32_t offset = (std::uint32_t) banks[window] * 0x1000u + (adr & 0x1FFFu);

		val = offset < file.rom.size() ? file.rom[offset] : 0;
		return true;
	}

	return xgm::NES_MEM::Read (adr, val, id);
}

bool NsfBus::Write (UINT32 adr, UINT32 val, UINT32 id)
{
	if (banked && adr >= 0x5FF8 && adr <= 0x5FFB)
	{
		banks[adr - 0x5FF8] = (std::uint8_t) (val & 0xFF);
		return true;
	}

	// Counted before dispatch, and counted even when the chip was never created: that is exactly
	// the case worth catching - the driver writing to a chip its header did not mention.
	if (adr >= 0x9000 && adr <= 0xB003) { ++expansionWrites[0]; }
	else if (adr == 0x9010 || adr == 0x9030) { ++expansionWrites[1]; }
	else if (adr == 0x4800 || adr == 0xF800) { ++expansionWrites[2]; }
	else if (adr == 0xC000 || adr == 0xE000) { ++expansionWrites[3]; }

	if (adr >= 0x4000 && adr <= 0x4017)
	{
		// The registers are the music. They leave the map here rather than being stored.
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
