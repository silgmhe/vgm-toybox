#pragma once

#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Ricoh RF5C68 and RF5C164: eight PCM channels that play samples out of a 64 KB RAM buffer.

	Nothing in the register file says which sample is playing - the start address is a register, but
	the data lives in the chip's RAM, which the VGM keeps in a separate 0xC0 block - so this decoder
	answers the one question the note extractor asks: whether a channel is sounding. Register 0x07
	bit 7 is the master output switch, and 0x08 is the per-channel mask with a cleared bit meaning
	enabled, exactly like the chip's own output gate.

	A sample has no pitch and no length the registers describe, so the gate is a fixed 80 ms after
	the channel is enabled - the same marker the OPN's ADPCM-A uses. That keeps a looping sample
	from holding a note until the end of the song; the note is closed by the timer, not by the
	driver, because a driver that leaves its channel enabled is the normal case.

	Not modelled, and why: the sample data itself (the addresses point into the chip's RAM block,
	not into anything this MIDI path can read), the end/loop addresses and the pan registers. */
class Rf5c68Decoder final : public IChipDecoder
{
public:
	enum Channel
	{
		channel0 = 0,
		channel1,
		channel2,
		channel3,
		channel4,
		channel5,
		channel6,
		channel7,
		numChannels
	};

	Rf5c68Decoder();
	~Rf5c68Decoder() override;

	void reset (std::uint32_t clock, double sampleRate) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	int channelCount() const override { return channelCount_; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	Family family (int channel) const override;
	bool isDrum (int channel) const override { return true; }

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	int channelCount_ = (int) Rf5c68Decoder::numChannels;
};

} // namespace toybox::chips
