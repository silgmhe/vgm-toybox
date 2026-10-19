#pragma once

#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The TI SN76489 and its clones: three square channels and a noise generator, each with a 4-bit
	attenuation instead of a volume.

	A write is one self-contained byte - latch (bit 7), channel and type (bits 6..4), data (bits
	3..0) - so the latch is part of the register file: a tone write needs a second byte to reach
	all ten divider bits, a volume write is complete on its own.

	Pitch is clock / (32 * period). The TI-made parts fold a divider of 0 to 0x400; the Sega parts
	the VGM format names keep it at 0, which stops the channel rather than playing the highest
	pitch, and that is the behaviour modelled here.

	The noise generator has four rates selected by 0xE0..0xE7. It has no musical pitch, so its
	modes map onto GM drum keys instead of a note.

	Not modelled, and why: the noise generator's phase reset on a mode change (it changes the
	timbre, not the note) and the Game Gear's stereo output mask (panning). */
class Sn76489Decoder final : public IChipDecoder
{
public:
	enum Channel
	{
		channel0 = 0,
		channel1,
		channel2,
		noiseChannel,
		numChannels
	};

	Sn76489Decoder();
	~Sn76489Decoder() override;

	void reset (std::uint32_t clock, double sampleRate) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	int channelCount() const override { return channelCount_; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	Family family (int channel) const override;
	bool isDrum (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	int channelCount_ = (int) Sn76489Decoder::numChannels;
};

} // namespace toybox::chips
