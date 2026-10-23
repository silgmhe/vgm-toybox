#pragma once

#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Yamaha YMZ280B (PCMD8): eight PCM/ADPCM voices reading from the song's ROM.

	Each voice has a pitch register, but it is a **playback rate** (0.172-88.2 kHz), not a note:
	the same value plays a kick drum and a flute sample at different musical pitches depending on
	what the sample was recorded at. Turning a rate into a note needs the sample's root rate and
	its loop points, which is the shared "step -> cents" problem in M4 of
	docs/desgin/midi-output-design.md - so this decoder reports a one-shot trigger, the same
	answer the OPN's ADPCM channels give.

	The gate is 80 ms from the key-on edge rather than the sample's end: the end address points
	into ROM, so a driver that leaves the voice keyed on (normal) would otherwise hold one note for
	the rest of the song.

	VGM's 0x5D carries the register number in `aa` and its value in `dd` - measured against real
	rips, not the offset-0/offset-1 latch pair the hardware's byte-wide bus would need. The core
	does that latching itself, so `write()` sees an already resolved register number. */
class Ymz280bDecoder final : public IChipDecoder
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

	Ymz280bDecoder();
	~Ymz280bDecoder() override;

	/** What this chip needs besides its register writes: the clock to assume when the song names
		none, and (for a sampled chip) the memory image its pitch is measured against. The assembler
		asks instead of carrying a table of these. */
	static constexpr ChipRequirements kRequirements { 16934400u };

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	int channelCount() const override { return channelCount_; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	ChannelKind channelKind (int channel) const override;
	bool isDrum (int channel) const override { return true; }

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	int channelCount_ = (int) Ymz280bDecoder::numChannels;
};

} // namespace toybox::chips
