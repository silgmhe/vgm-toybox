#pragma once

#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The WonderSwan / WonderSwan Color sound chip (libvgm's "in_wsr" core): four channels, each a
	32-step 4-bit waveform stepped every 32 * (2048 - frequency) clocks, so - as on the SCC and the
	HuC6280 - the register counts down in pitch and 0 is the lowest note.

	There is no key-on bit: a channel sounds while its bit in 0x90 is set and its volume is above
	zero, and the note ends when the driver clears the bit.

	Channel 1 can be taken over by the voice mode (0x90 bit 5), which streams PCM bytes instead of
	a waveform. The bytes arrive through the sound DMA, so there is no pitch to report - the channel
	becomes a one-shot marker on C4, the way the YM2612 DAC channel is.

	Channel 3 can switch to the LFSR noise generator (0x90 bit 7). The generator shifts at the
	channel's waveform step rate, so the noise has no pitch of its own and its rate picks a GM drum
	key.

	Channel 2 has a sweep unit (0x8C step, 0x8D interval, 0x90 bit 6) that moves the pitch while no
	register is written, which is why this decoder needs the sample rate and advance().

	Not modelled, and why: the waveform RAM (the shape does not change which note sounds), the
	master volume (0x91) and the PCM volume (0x94) beyond the voice marker's velocity, and the LFSR
	tap selection (0x8E) beyond "this channel is noise". */
class WswanDecoder final : public IChipDecoder
{
public:
	enum Channel
	{
		channel0 = 0,
		channel1,		// the voice / sound-DMA channel
		channel2,		// the sweep channel
		channel3,		// the noise channel
		numChannels
	};

	WswanDecoder();
	~WswanDecoder() override;

	/** What this chip needs besides its register writes: the clock to assume when the song names
		none, and (for a sampled chip) the memory image its pitch is measured against. The assembler
		asks instead of carrying a table of these. */
	static constexpr ChipRequirements kRequirements { 3072000u };

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (uint32_t sampleTime) override;

	int channelCount() const override { return channelCount_; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	ChannelKind channelKind (int channel) const override;
	bool isDrum (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;

	/** 0x8D counts sweep ticks in HBlanks; turning that into samples needs the clock. */
	void setSweepTime (uint8_t value, uint32_t sampleTime);

	void stepSweep (uint32_t sampleTime);

	int channelCount_ = (int) WswanDecoder::numChannels;
};

} // namespace toybox::chips
