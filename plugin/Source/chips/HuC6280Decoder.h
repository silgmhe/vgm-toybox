#pragma once

#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Hudson HuC6280 (PC Engine / TurboGrafx-16) PSG: six channels, each a 32-entry 5-bit
	wavetable fed by a 12-bit frequency divider.

	Unlike the 2A03/DMG decoders this chip has no timers of its own: a note lasts exactly as long
	as the driver leaves the channel enabled, so a register write is already the whole story.

	Channels 4 and 5 can switch their wavetable for a pseudo-random noise generator (register 0x07
	bit 7). The noise has no pitch, so its rate selects a GM drum key the way the 2A03 noise
	channel does; the switch can happen at any time, which is why the period carries a flag bit
	instead of the channel owning two note tables.

	Not modelled, and why: the global and per-channel balance registers only scale left/right and
	a MIDI note has no place for them; the LFO (0x08/0x09) modulates channel 0's frequency from
	channel 1's waveform and would turn this into a second state machine; DDA mode (control bit 6)
	streams samples instead of a tone, so the channel is reported as silent. */
class HuC6280Decoder final : public IChipDecoder
{
public:
	enum Channel
	{
		channel0 = 0,
		channel1,
		channel2,
		channel3,
		channel4,		// the first of the two channels that can switch to noise
		channel5,
		numChannels
	};

	HuC6280Decoder();
	~HuC6280Decoder() override;

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	int channelCount() const override { return channelCount_; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	ChannelKind channelKind (int channel) const override;
	bool isDrum (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	int channelCount_ = (int) HuC6280Decoder::numChannels;
};

} // namespace toybox::chips
