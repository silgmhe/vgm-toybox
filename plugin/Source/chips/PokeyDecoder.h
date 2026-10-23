#pragma once

#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Atari POKEY: four channels, each an 8-bit divider plus a 4-bit volume and a distortion
	selector, and three clock modes that AUDCTL switches between - which is what makes it more
	than four independent dividers:

	- 64 kHz (the 1.79 MHz clock divided by 28): output = clock / (28 * 2 * (divisor))
	- 15 kHz (AUDCTL bit 0, divided by 114): output = clock / (114 * 2 * (divisor))
	- 1.79 MHz (AUDCTL bits 6/5 for channel 1/3): the channel counts CPU clocks, and the core
	  adds four clocks to the divider (seven in 16-bit mode)
	- 16-bit pairs (AUDCTL bits 4/3): channel 2 extends channel 1 (or 4 extends 3) into one
	  divider of AUDF_low + 256 * AUDF_high + 7, so the pair is a single voice

	A joined pair is reported on its low channel only: the high channel's divider is the second
	byte of the 16-bit value, not a voice of its own.

	Not modelled: the distortion selector (4/5/9/17-bit noise at the divider rate), so a noise
	channel reports the divider's rate as its pitch; AUDC bit 4 ("volume only") outputs a DC level
	and is reported as silent. */
class PokeyDecoder final : public IChipDecoder
{
public:
	enum Channel
	{
		channel0 = 0,
		channel1,
		channel2,
		channel3,
		numChannels
	};

	PokeyDecoder();
	~PokeyDecoder() override;

	/** What this chip needs besides its register writes: the clock to assume when the song names
		none, and (for a sampled chip) the memory image its pitch is measured against. The assembler
		asks instead of carrying a table of these. */
	static constexpr ChipRequirements kRequirements { 1789772u };

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	int channelCount() const override { return channelCount_; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	ChannelKind channelKind (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	int channelCount_ = (int) PokeyDecoder::numChannels;
};

} // namespace toybox::chips
