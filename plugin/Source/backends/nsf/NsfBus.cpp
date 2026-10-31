#include "backends/nsf/NsfBus.h"

#include <cstdint>

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
const uint8_t kPlayerProgram[] =
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
		banks[i] = declared && i < file.bankSwitch.size () ? file.bankSwitch[i] : (uint8_t) i;
	}
}

void NsfBus::load (void)
{
	// SetImage clears the whole 64 KB before copying, so anything written before it is lost - the
	// trampoline in particular. It goes in afterwards, every time.
	resetBanks ();
	SetImage (const_cast<uint8_t*> (file.rom.data()), file.loadAddress, (uint32_t) file.rom.size());
	SetReserved (kPlayerProgram, (uint32_t) sizeof (kPlayerProgram));

	// A restart is a new song as far as a sample is concerned: the previous selection describes
	// bytes the driver is about to overwrite in its own image, and showing them would be showing
	// the last song's drum on the first frame of this one.
	dpcm = DpcmSelection ();
	pendingDpcmPage = 0;
	pendingDpcmLength = 0;
	pendingDpcmRate = 0;
	pendingDpcmLoop = false;
	dpcmEnabled = false;
}

uint8_t NsfBus::peek (uint16_t address) const
{
	uint32_t value = 0;

	// Read() stores nothing - it maps a bank or asks the image - so the cast describes the shape of
	// the interface rather than escaping const. Every counter on this class is bumped by Write().
	const_cast<NsfBus*> (this)->Read (address, value, 0);

	return (uint8_t) (value & 0xFF);
}

bool NsfBus::Read (uint32_t adr, uint32_t& val, uint32_t id)
{
	if (banked && adr >= 0x8000 && adr <= 0xFFFF)
	{
		// Eight 4 KB windows, selected one register each: $5FF8 is the one at $8000 and $5FFF the
		// one at $F000. Bank numbers are counted in the same 4 KB units.
		const uint32_t window = (adr >> 12) & 0x07u;
		// The windows are counted from the load address, which is not required to be 4 KB aligned:
// a file loaded at \ puts its first byte at window 0 offset \, not at offset 0.
		const uint32_t bankBase = (uint32_t) (file.loadAddress & 0x0FFF);
		const uint32_t offset = (uint32_t) banks[window] * 0x1000u + (adr & 0x0FFFu) - bankBase;

		val = offset < file.rom.size() ? file.rom[offset] : 0;
		return true;
	}

	return xgm::NES_MEM::Read (adr, val, id);
}

bool NsfBus::Write (uint32_t adr, uint32_t val, uint32_t id)
{

	if (adr == 0x0211)
	{
		++gateWrites;
		gateValue = (uint8_t) (val & 0xFF);
	}

	if (adr >= 0x0200 && adr <= 0x02FF)
	{
		++ramWriteCount;
	}
	if (banked && adr >= 0x5FF8 && adr <= 0x5FFF)
	{
		banks[adr - 0x5FF8] = (uint8_t) (val & 0xFF);

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
	else if (adr >= 0x5000 && adr <= 0x5015)
	{
		// The MMC5's audio registers live in the CPU's own map, not in an expansion window; the
		// write still has to leave the map or the chip is ticked and never told anything.
		expansion = true;
	}

	else if (adr == 0x4023 || (adr >= 0x4040 && adr <= 0x408A))
	{
		// The FDS keeps its register file - wavetable included - at $4040-$408A rather than in an
		// expansion window, and its master switch at $4023. Not counted in expansionWrites: that
		// tally is what detectUndeclaredChip reads, and the FDS declares itself in the header.
		expansion = true;
	}
	else { expansion = false; }

	if (expansion || (adr >= 0x4000 && adr <= 0x4017))
	{
		// The DPCM's three registers, remembered because nothing on the chip reports them back.
		// NES_DMC hands out the address and the rate through its track info but not the length,
		// and a file written from the sample needs the length to know where it ends.
		//
		// Latched on the write that starts the sample rather than on the register writes: a driver
		// may point $4012/$4013 at the next sample while the current one is still playing.
		if (adr == 0x4011)
		{
			// The DMC's DAC, written directly - the one place a drum's decay could come from, since
			// the data itself is a stream of equal steps. Counted so the probe can answer whether a
			// song uses it; the value is not kept, because nothing reads it back.
			++dacWrites;
		}
		else if (adr == 0x4010)
		{
			// The rate index and the loop bit. Remembered with the rest because a driver is free
			// to restore this register after a hit, and then the register describes the next
			// sample rather than the one being heard.
			pendingDpcmRate = (uint8_t) (val & 0x0F);
			pendingDpcmLoop = (val & 0x40) != 0;
		}
		else if (adr == 0x4012)
		{
			pendingDpcmPage = (uint8_t) (val & 0xFF);
		}
		else if (adr == 0x4013)
		{
			// All eight bits, which is what the chip this emulates uses: the core computes
			// `dlength = (len_reg << 4) + 1` from the full register, so a driver that writes
			// 0x2B asks for 689 bytes. Reading only the low four bits - the real 2A03's width -
			// cut every sample longer than 241 bytes down to a quarter and made a snare look like
			// a click. NotSoFatso does the same thing, which is why FamiStudio and the player
			// disagreed with the exporter.
			pendingDpcmLength = (uint8_t) (val & 0xFF);
		}
		else if (adr == 0x4015)
		{
			const bool enabled = (val & 0x10) != 0;

			// Only the edge starts a sample, which is what the hardware does. A driver that keeps
			// the bit set and writes $4015 every frame for the pulse channels would otherwise
			// latch again on each of those writes, and what it latched would be the registers as
			// they happened to stand - which is a page of silence rather than the drum.
			if (enabled && ! dpcmEnabled)
			{
				dpcm.start = (uint16_t) (0xC000u + (uint32_t) pendingDpcmPage * 64u);

				// Eight bits of length, sixteen bytes each, plus the one padding byte: the same
				// arithmetic the core does at nes_dmc.cpp:622 and NotSoFatso at NSF_Core.cpp:321.
				// 4081 is the largest it can name, and the copy is a 4KB window, so it fits.
				dpcm.length = (uint16_t) ((uint32_t) pendingDpcmLength * 16u + 1u);
				dpcm.rateIndex = pendingDpcmRate;
				dpcm.loop = pendingDpcmLoop;
				dpcm.valid = true;
				++dpcmStarts;

				// Recorded here rather than on every $4012 write: a driver prepares the next
				// sample's address before it starts it, and an address that was written and never
				// started is not a sample. It was the boundary search's input once - an entry that
				// never played cut a real sample short - and the probe still reads it.
				bool known = false;

				for (uint32_t i = 0; i < pageCount; ++i)
				{
					known = known || pagesSeen[i] == pendingDpcmPage;
				}

				if (! known && pageCount < pagesSeen.size ())
				{
					pagesSeen[pageCount++] = pendingDpcmPage;
				}

				// The bytes, taken now while the bank window still points at the sample. Reading
				// them later would describe whatever the driver pointed that window at afterwards,
				// which is a picture of the code rather than of the drum - and it is the same
				// address, the same length and the same rate, so nothing else gives it away.
				//
				// A whole window rather than `dpcm.length`: that register caps at 241 bytes, and the
				// samples here are longer than that. Where the copy ends is decided later, by the
				// next sample's address.
				for (std::size_t i = 0; i < dpcm.bytes.size (); ++i)
				{
					const auto address =
						(uint16_t) (0x8000u + ((uint32_t) (dpcm.start - 0x8000u) + i) % 0x8000u);

					dpcm.bytes[i] = peek (address);
				}
			}

			dpcmEnabled = enabled;
		}

		// The registers are the music, whichever chip owns them. They leave the map here rather
		// than being stored - and an expansion write has to leave it too, or the chip is created,
		// ticked, mixed and never told anything.
		if (onSoundWrite)
		{
			onSoundWrite ((uint16_t) adr, (uint8_t) (val & 0xFF));
		}

		++soundWriteCount;
		return true;
	}

	return xgm::NES_MEM::Write (adr, val, id);
}

} // namespace toybox::backends::nsf
