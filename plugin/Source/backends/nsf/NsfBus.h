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
		write that merely falls inside the range. This is what the detection judges by, and one
		register per chip is enough: $9000-$9003 for VRC6 (control and pitch of the first pulse),
		$9010 or $9030 for VRC7, $4800 or $F800 for N163, $C000 or $E000 for FME7. */
	std::array<uint32_t, 4> expansionSignature {};

	/** One byte of the address space, along the same path the CPU would take.

		Not const, because Read() is not: the base class maps a bank on read, and a wrapper that
		promised not to would have to cast the promise away again. It does not write, so nothing
		observable changes - but the signature is honest about which function it is calling.
	*/
	uint8_t peek (uint16_t address);

	/** The DPCM sample the last start selected.

		Latched at the write that starts it rather than read off the registers afterwards: a driver
		is free to write the next sample's address and length while the current one is still playing,
		and the register would then describe a sample nobody is hearing.
	*/
	struct DpcmSelection
	{
		uint16_t start = 0;		// absolute CPU address: $C000 + page * 64
		uint16_t length = 0;	// bytes: (register * 16) + 1, up to 4081
		uint8_t rateIndex = 0;	// $4010 bits 0..3, which pick the read rate
		bool loop = false;		// $4010 bit 6
		bool valid = false;

		/** The bytes as they stood at the moment the sample started, from `start` onwards.

			Copied here rather than read when someone asks for them, because the window $C000 lives
			in is a bank the driver owns: it can point that window at something else the instant the
			hit is over, and a later read then describes whatever the driver put there instead. What
			that reads as is noise with no envelope - the shape of code, not of a drum.

			A whole window, four kilobytes, because that is the reach of the register that names a
			sample: eight bits counting sixteen-byte units, plus the padding byte, comes to 4081.
			Sized from that range and not from the length any one song happens to ask for - 193 was
			one of those, and it wrote past the end of this array on the first song that asked for
			more.
		*/
		std::array<uint8_t, 4096> bytes {};
	};

	DpcmSelection dpcm;

	/** How many samples have started since the load.

		A drum that sounds longer than one sample can be a driver retriggering it, and this count is
		what tells that apart from a sample that really is longer. It is also half of the wave
		cache's key: the same address played twice reads different bytes if the driver moved the bank
		in between, and only the count distinguishes the two plays.
	*/
	uint32_t dpcmStarts = 0;

	/** Writes to $4011, the DMC's DAC.

		This is where a NES drum's decay could live - the data is a stream of fixed-size steps with
		no envelope of its own, so the volume would come from the driver walking the DAC down while
		the sample plays. Measured on the songs here: two writes each, so it is not used that way and
		a sample exported without it loses nothing.

		Kept as a probe counter, because that measurement is the only way to know, and the next song
		that sounds wrong will ask the same question.
	*/
	uint32_t dacWrites = 0;

	/** Every distinct $4012 page that actually started a sample, in order.

		Read by the probe, and by nothing else: where a sample ends is the register's answer now,
		rather than the distance to the next page. It stays because it is how "this song has one
		sample" and "this song has four" were told apart, and that question will come back.
	*/
	std::array<uint8_t, 16> pagesSeen {};
	uint32_t pageCount = 0;

	/** One distinct sample, kept whole at the moment it started.

		The window is a copy rather than an address because the bank is the driver's to change: by
		the time anyone asks, `$C000` may point at something else entirely, and the bytes would
		describe the code rather than the drum.
	*/
	struct DpcmSample
	{
		uint16_t start = 0;
		uint16_t length = 0;		// as the register names it; the tail still needs trimming
		uint8_t rateIndex = 0;
		std::array<uint8_t, 4096> bytes {};
	};

	/** How many distinct samples are kept. Sixteen windows is 64 KB of resident memory, which is
		a song's worth - a file that starts more than this is re-pointing one window rather than
		carrying sixteen samples.
	*/
	static constexpr std::size_t kMaxSamples = 16;

	/** The distinct samples started so far, in the order they first played.

		Grown by Write() on the audio thread and read by the exporter on another, so it is a plain
		array and a counter rather than a vector: the writer fills the entry before it raises the
		count, and the count only ever rises, which is enough for a reader that takes a copy.
	*/
	std::array<DpcmSample, kMaxSamples> samples {};
	uint32_t sampleCount = 0;

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
