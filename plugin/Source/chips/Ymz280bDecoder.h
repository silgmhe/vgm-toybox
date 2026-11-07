#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Yamaha YMZ280B: eight ADPCM/PCM voices reading a sample ROM.

	**These rips use it as a streamer, not as a chip with eight parts.** Measured on the ten songs in
	`test_file/YMZ280B`: voices 1-7 are written exactly once each (fnum, control, level, pan - all
	zero) and never touched again, while voice 0 gets one long start..stop range that the driver
	keeps **rewriting forward** as it feeds the next section. There is no per-note register traffic
	at all - a whole song is roughly fifty writes.

	So a row here means "a voice playing its slice of the ROM", not "an instrument with a pitch".
	That is why the row reports a rate and a shape from memory rather than a note.

	The registers (`ymz280b.c:826-930`), four blocks of four, with the voice in bits 2-4:

	  0x00 fnum low         0x01 control: bit7 key-on, bit4 loop, bits 5-6 mode, bit0 fnum bit 8
	  0x02 level            0x03 pan
	  0x20/0x40/0x60        start address high / middle / low, and the same for loop start,
	                        loop end and stop - all in **nibbles**

	**Playback also starts from a global register.** `0xFF` bit 7 is the key-on *enable*: when it
	rises, every voice that is already configured with bit 7 of its control **and** looping starts at
	once (`ymz280b.c:985-992`). The stream does exactly that - it writes `0xFF = 0xD0` first, then
	`0x01 = 0xB0` for the voice itself - so a decoder that only watches per-voice key-ons misses the
	first start entirely.

	The ROM arrives as data block `0x86`, a plain ROM block with a size/offset header. */
class Ymz280bDecoder final : public IChipDecoder
{
public:
	Ymz280bDecoder();
	~Ymz280bDecoder() override;

	Ymz280bDecoder (const Ymz280bDecoder&) = delete;
	Ymz280bDecoder& operator= (const Ymz280bDecoder&) = delete;

	/** What this chip needs besides its register writes: a clock to fall back on - and **the sample
		ROM**, which block `0x86` carries with a size/offset header, so this is not a RAM upload. */
		static constexpr ChipRequirements kRequirements { 16934400u, true, 0x86, 512u * 1024u, false, false };

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

	/** Hands the chip its sample ROM. Without it a voice still keys on - the registers say so - but
		there are no bytes to draw, so the row stays a trigger. The image survives a `reset()`. */
	void setRom (std::vector<uint8_t> rom);

	static constexpr int numChannels = 8;

	int channelCount() const override { return channelCount_; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	ChannelKind channelKind (int channel) const override;

	/** Every voice here is a one-shot recording, so the row is a drum part rather than a pitch -
		the same answer GA20 and the ES5503 give their PCM channels. */
	bool isDrum (int channel) const override { (void) channel; return true; }

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	int channelCount_ = (int) Ymz280bDecoder::numChannels;
};

} // namespace toybox::chips
