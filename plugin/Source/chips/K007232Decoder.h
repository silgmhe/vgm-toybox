#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Konami K007232: two PCM voices with a real pitch register.

	The core runs at `clock / 128` (`K007232_CLOCKDIV`, k007232.c:50) and walks a sample by
	decrementing a counter thirty-two at a time per output sample, adding `0x1000 - step` whenever it
	passes zero (k007232.c:236-238). In the steady state that is

		steps per sample = 32 / (0x1000 - step)
		frequency        = clock / (4 * (0x1000 - step))

	so a larger `step` is a higher note, and `step` 0 is the floor of the range.

	Six registers per voice, voice `n` at `6 * n` (k007232.c:265), then board registers that every
	voice shares:

	  0x00/0x06  pitch, low byte
	  0x01/0x07  pitch, high byte: bits 4-5 pick a frequency mode, the low nibble is the high bits
	             of the twelve-bit step in the normal mode
	  0x02..0x04 / 0x08..0x0A  start address, low to high
	  0x05/0x0B  key on/off - bit 0 is the key, which is this decoder's gate
	  0x0C       external port (volume/pan nibble, not modelled)
	  0x0D       loop enable, one bit per voice
	  0x10..0x13  left/right volume per voice
	  0x14/0x15  **bank**, one per voice: `data << 17`

	**The sample's bytes.** `k007232.c:232` reads `out = (value & 0x7F) - 0x40`, so the data is
	seven-bit unsigned with `0x40` as silence - not a delta codec, and not eight-bit signed. The
	length is not in a register either: `k007232.c:246` restarts the voice when a byte has **bit 7
	set**, so the terminator lives in the data. Measured on `02 Title Demo`: 2 such bytes in a
	4752-byte block and 1 in a 5557-byte one, which is a terminator per sample rather than the half
	a signed format would put there.

	**The address is `bank + start`.** `start` is seventeen bits (`K007232_ADDR_MASK`) and the bank
	register shifts by seventeen more, so the two voices of one song read different 128 KB windows
	of the same image - measured as `0x14=1, 0x15=3` on `02 Title Demo`, matching the ROM blocks at
	0x20000 and 0x60000.

	The pitch comes from `step`, not from the bytes, which makes this `registerExact` with no root
	layer - but the *rate* a file has to declare is per note, because the chip has no single sample
	rate: `(clock / 128) * 32 / (0x1000 - step)` bytes per second. */
class K007232Decoder final : public IChipDecoder
{
public:
	K007232Decoder();
	~K007232Decoder() override;

	K007232Decoder (const K007232Decoder&) = delete;
	K007232Decoder& operator= (const K007232Decoder&) = delete;

	/** The clock to assume when the song names none, and the ROM its voices read. `0x94` is the
		VGM block type for this chip; the four blocks of one song merge by their declared offsets
		into a single image, and the bank registers pick the window. */
	static constexpr ChipRequirements kRequirements { 8000000u, true, 0x94, 8u << 20, false, false };

	void attachMemory (const ChipMemory& memory) override
	{
		if (memory.bytes != nullptr && ! memory.bytes->empty())
		{
			setRom (*memory.bytes);
		}
	}

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (uint32_t sampleTime) override;

	int channelCount() const override { return 2; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	ChannelKind channelKind (int channel) const override;

	/** The ROM the voices read. A copy, because the assembler's image lives for one flush. */
	void setRom (std::vector<uint8_t> rom);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace toybox::chips
