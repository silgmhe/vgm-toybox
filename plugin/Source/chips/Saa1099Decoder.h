#pragma once

#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Philips SAA1099: six square channels in three octave pairs, two noise generators and two
	envelope generators.

	Pitch is an 8-bit frequency register per channel plus a 3-bit octave shared by a pair of
	channels; the output frequency is clock * 2^octave / (512 * (511 - frequency)). The core
	divides the master clock by 256 to get its sample rate, and a half wave lasts
	(511 - frequency) / 2^octave of those samples.

	Sound only reaches the output when the chip-wide enable (register 0x1C, bit 0) is set, so a
	driver has to switch it on before any channel counts as audible - that is a property of the
	chip, not a driver habit.

	A channel whose tone generator is off and whose noise is on has no pitch: its divider is
	whatever the driver last left there, so the note comes from the noise generator's rate instead
	and lands on a drum key. The three fixed rates are one octave apart, the fourth follows the
	tone divider - only that order is kept, because the generator's actual frequencies are far
	above anything a note could name.

	Not modelled, and why: the left/right amplitude nibbles (panning, no MIDI place for it) and the
	envelope generators (they reshape the volume over time, so a note may outlive what the envelope
	would have done). */
class Saa1099Decoder final : public IChipDecoder
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
		numChannels
	};

	Saa1099Decoder();
	~Saa1099Decoder() override;

	/** What this chip needs besides its register writes: the clock to assume when the song names
		none, and (for a sampled chip) the memory image its pitch is measured against. The assembler
		asks instead of carrying a table of these. */
	static constexpr ChipRequirements kRequirements { 8000000u };

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
	int channelCount_ = (int) Saa1099Decoder::numChannels;
};

} // namespace toybox::chips
