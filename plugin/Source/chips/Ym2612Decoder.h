#pragma once

#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The YM2612 (OPN2): six four-operator FM channels in two ports.

	An FM channel is not "on" in the sense a PSG channel is. It sounds only while the operators
	that reach the output - the carriers of its current algorithm - have a total level below
	maximum, so a patch whose signal path ends in a silenced carrier is silent even though the
	key register says otherwise. Tracking the algorithm and the four total levels is what makes
	this decoder worth having: the old path treated every 0x28 key-on as a note.

	Pitch follows the hardware relation f = FNUM * clock / (144 * 2^(20 - BLOCK)), with 144 the
	FM sample-clock divisor shared by the OPN family (12 * prescaler 6 * 2).

	The DAC (register 0x2B bit 7) replaces channel 6's output with a sample stream; that channel
	is reported silent while the DAC is enabled, because its notes come from the sample track
	rather than from the FM operators. */
class Ym2612Decoder final : public IChipDecoder
{
public:
	static constexpr int numChannels = 6;

	Ym2612Decoder();
	~Ym2612Decoder() override;

	void reset (std::uint32_t clock, double sampleRate) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	int channelCount() const override { return numChannels; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	Family family (int channel) const override { return Family::fm; }

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace toybox::chips
