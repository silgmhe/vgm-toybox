#pragma once

#include <cstdint>
#include <memory>

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

	Six registers per voice, voice `n` at `6 * n` (k007232.c:265):

	  0x00/0x06  pitch, low byte
	  0x01/0x07  pitch, high byte: bits 4-5 pick a frequency mode, the low nibble is the high bits
	             of the twelve-bit step in the normal mode
	  0x02..0x04 / 0x08..0x0A  start address, low to high
	  0x05/0x0B  key on/off - bit 0 is the key, which is this decoder's gate

	The sample's own bytes only decide timbre and length; the pitch comes from `step`, which makes
	this `registerExact` with no root layer. */
class K007232Decoder final : public IChipDecoder
{
public:
	K007232Decoder();
	~K007232Decoder() override;

	K007232Decoder (const K007232Decoder&) = delete;
	K007232Decoder& operator= (const K007232Decoder&) = delete;

	/** What this chip needs besides its register writes: the clock to assume when the song names
		none, and (for a sampled chip) the memory image its pitch is measured against. The assembler
		asks instead of carrying a table of these. */
	static constexpr ChipRequirements kRequirements { 8000000u };

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (uint32_t sampleTime) override;

	int channelCount() const override { return 2; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	ChannelKind channelKind (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace toybox::chips
