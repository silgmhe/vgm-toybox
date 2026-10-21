#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace toybox::backends::libvgm
{

/** One command of a VGM command stream, already aligned and stripped of its length.

	The reader owns the format knowledge (header offsets, command lengths, wait commands) so
	that the MIDI builder only ever sees "this chip register was written at this sample". The
	device mapping is deliberately *not* part of this struct: which libvgm device answers a
	command is the caller's decision, so a stream can be replayed against any device layout.

	One stream byte can be two facts: 0x80-0x8F writes a byte of the song's PCM bank to the
	YM2612's register 0x2A *and* waits n samples. Those are reported as two commands - a
	register write carrying `command = 0x52` (the format's own name for a YM2612 port 0 write)
	at the write's time, followed by the wait it came with. */
struct VgmCommand
{
	enum class Kind
	{
		registerWrite,	// addr/data carry the payload, port the second-chip flag
		registerWritePair,	// K051649: addr is the even offset, data its value, data2 the byte
							// the chip receives at addr | 1 (the VGM command carries both)
		wait,			// sampleTime is the command's own time; the clock has been advanced
						// (0x61-0x63, 0x70-0x7F, and 0x80-0x8F - the YM2612 DAC stream's waits)
		end,			// 0x66 - the caller should stop
		ignored			// data block, seek, or an unknown command
	};

	Kind kind = Kind::ignored;
	std::uint8_t command = 0;		// the raw VGM command byte
	std::uint8_t port = 0;			// 0 / 1 - the second chip of a pair (bit 7 of the address byte);
									// for 0xC8 (X1-010) it carries the high byte of the 16-bit offset
	std::uint8_t addr = 0;			// register / index byte
	std::uint8_t data = 0;			// value byte
	std::uint8_t data2 = 0;			// second value byte of a pair write; for 0xE1 (C352) the
									// low byte of the 16-bit value, `data` being the high one
	std::uint32_t sampleTime = 0;	// absolute position of the command, in 44100 Hz samples
};

/** One 0x67 data block, still where it sits in the image.

	The block's `type` byte decides what the bytes are: 0x00 is the YM2612's PCM bank, 0x8D the
	C140's sample ROM, the bit 0x80 says the block came from a ROM image that is being uploaded,
	and only those blocks carry the eight-byte ROM header (`romSize`, `romOffset`). A chip whose
	samples are recorded rather than synthesised needs them: the note of a sample is its playback
	rate divided by the sample's own cycle length, and that length is only in these bytes. */
struct VgmDataBlock
{
	std::uint8_t type = 0;			// the block's type byte, the ROM flag (0x80) included
	std::uint32_t romSize = 0;		// bytes of the ROM the block belongs to (ROM blocks only)
	std::uint32_t romOffset = 0;	// where in that ROM the payload belongs (ROM blocks only)
	std::uint32_t ramOffset = 0;	// ...and where in the chip's RAM (0xC0-0xFF blocks, which
									// libvgm's `_VGM_RAM_CHIPS` table names: 16-bit address below
									// 0xE0, 32-bit from 0xE0)
	const std::uint8_t* data = nullptr;
	std::size_t size = 0;			// payload bytes, the ROM header and the RAM address not counted
};

/** Walks one uncompressed VGM image. The stream ends at the EOF offset, the GD3 tag or the
	0x66 command, whichever comes first - the same boundaries libvgm uses. */
class VgmCommandReader
{
public:
	VgmCommandReader (const void* data, std::size_t size);

	/** True while the stream is valid; next() is only meaningful while valid. */
	bool isValid (void) const { return data != nullptr && position < end; }

	/** Advances past one command. Returns false at the end of the stream. */
	bool next (VgmCommand& command);

	/** The clock after everything read so far, in 44100 Hz samples. A whole stream read through
		ends on the header's total, which is the length the engine plays - so a difference here is
		a difference between MIDI time and audio time. */
	std::uint32_t timeSamples (void) const { return sampleTime; }

	/** Every 0x67 block the stream read so far, in stream order. The bytes stay in the image, so
		this is cheap; a caller that needs a chip's ROM assembles it from the blocks it recognises. */
	const std::vector<VgmDataBlock>& dataBlocks (void) const { return blocks; }

	/** The C140's ROM banking, from the header's byte 0x96 - the same field libvgm hands to the
		core as `DEV_GEN_CFG::flags` (vgmplayer.cpp, `DEVID_C140`). It is not a register and not
		a data block: it says how the board folds a voice's bank and address into a ROM offset, so
		a decoder cannot read a sample without it. Header byte 0x96 only exists once the header
		reaches that far (VGM 1.61 and later); an older file has a command there, so the answer
		is the format's default, 0x00 (Namco System 2), rather than whatever byte happens to sit
		at that offset. */
	std::uint8_t c140Banking (void) const { return c140BankingType; }

private:
	/** One 0x67 data block that belongs to the YM2612 PCM bank. The bytes stay where they are in
		the image; the bank is the blocks in the order they were read, which is what 0x8n consumes. */
	struct BankSegment
	{
		const std::uint8_t* data = nullptr;
		std::size_t size = 0;
	};

	bool readHeader (void);

	/** Hands out the next byte of the PCM bank, or false once the bank is used up. */
	bool readBankByte (std::uint8_t& value);

	const std::uint8_t* data = nullptr;
	std::size_t size = 0;
	std::size_t position = 0;
	std::size_t end = 0;
	std::uint32_t sampleTime = 0;

	std::vector<BankSegment> pcmBank;
	std::size_t bankPosition = 0;
	std::vector<VgmDataBlock> blocks;
	std::uint8_t c140BankingType = 0x00;		// System 2, the format's default

	// 0x8n is one byte that stands for two commands; the second one waits here until the caller
	// asks for it, so a stream that ends right after a 0x8n still reports its wait.
	VgmCommand resume;
	bool hasResume = false;
};

} // namespace toybox::backends::libvgm
