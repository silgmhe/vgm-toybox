#pragma once

#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Konami K051649 (SCC1, and the K052539 SCC+ variant): five wavetable channels of 32
	4-bit samples each.

	The chip has a latch: a write to an even offset sets the register index, the following write
	to the odd offset carries the value. Frequency is 12 bits, volume 4 bits and the key-on mask
	one bit per channel - a driver usually writes the mask once and then only changes pitches.

	The core halts a channel whose frequency register is 8 or below (a divider of nine clocks or
	less never completes a wave cycle), which is how "silence" starts here as well as a key-on.

	The row draws the channel's **waveform RAM**: thirty-two signed 8-bit samples, walked at
	`2 * clock / (f + 1)` steps a second (the tone is that over thirty-two). The two chips address
	that RAM differently - SCC shares one table between channels 3 and 4, SCC+ gives channel 4 its
	own half - so `modePlus`, from `ChipConfig::flags` bit 0, decides how a write is placed. The flag
	is the VGM clock field's bit 31, the same bit libvgm reads to name the chip K052539.

	Not modelled: the test register (it only gates writes, and its deform mode is not emulated by
	the playback core either - so drawing a deformed table would disagree with the audio) and the
	bus conflicts the core's own TODO lists. */
class K051649Decoder final : public IChipDecoder
{
public:
	enum Channel
	{
		channel0 = 0,
		channel1,
		channel2,
		channel3,
		channel4,
		numChannels
	};

	K051649Decoder();
	~K051649Decoder() override;

	/** What this chip needs besides its register writes: the clock to assume when the song names
		none, and (for a sampled chip) the memory image its pitch is measured against. The assembler
		asks instead of carrying a table of these. */
	static constexpr ChipRequirements kRequirements { 3579545u };

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (uint32_t sampleTime) override;

	int channelCount() const override { return channelCount_; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	ChannelKind channelKind (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	int channelCount_ = (int) K051649Decoder::numChannels;
};

} // namespace toybox::chips
