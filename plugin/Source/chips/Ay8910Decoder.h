#pragma once

#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The General Instruments AY-3-8910 and its clones (YM2149, YM2203/YM2608/YM2610's SSG): three
	square channels, one noise generator and one envelope generator.

	Pitch is clock / (16 * period) from the twelve-bit tone divider in registers 0..5; the mixer
	(register 7) disables the tone with a set bit, so a cleared bit is what makes the channel sound.

	Register 8..A bit 4 turns a channel over to the envelope generator. The envelope's level is not
	modelled - only the fact that it is running, which keeps the channel audible: the shape and its
	period would reshape the volume over time, but the note boundaries come from the tone divider
	and the mixer, not from the envelope.

	The noise generator does not get a track of its own: libvgm's AY has three channels, so a
	channel that plays noise instead of its tone reports no note. */
class Ay8910Decoder final : public IChipDecoder
{
public:
	enum Channel
	{
		channel0 = 0,
		channel1,
		channel2,
		numChannels
	};

	Ay8910Decoder();
	~Ay8910Decoder() override;

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
	int channelCount_ = (int) Ay8910Decoder::numChannels;
};

} // namespace toybox::chips
