#pragma once

#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Game Boy DMG APU: two pulses, a 32-step wave channel and a noise channel.

	Built like the 2A03 decoder for the same reason: a register write alone does not say how long
	a note lasts, the frame sequencer does. It runs at 512 Hz off the CPU clock and clocks the
	length counters (256 Hz), the sweep (128 Hz) and the envelopes (64 Hz), so a note ends when
	the chip silences the channel, not when the driver remembers to key it off.

	The wave channel is a sample player, but a Game Boy driver streams its 32 samples at whatever
	pitch the song needs, so it is decoded like a tone channel. */
class GbcApuDecoder final : public IChipDecoder
{
public:
	enum Channel
	{
		pulse1 = 0,
		pulse2,
		wave,
		noise,
		numChannels
	};

	GbcApuDecoder();
	~GbcApuDecoder() override;

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (uint32_t sampleTime) override;

	int channelCount() const override { return channelCount_; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	ChannelKind channelKind (int channel) const override;
	bool isDrum (int channel) const override { return channel == noise; }

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	int channelCount_ = (int) GbcApuDecoder::numChannels;
};

} // namespace toybox::chips
