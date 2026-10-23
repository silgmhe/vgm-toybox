#pragma once

#include <cstdint>
#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** YM2151 (OPM): eight four-operator FM channels.

	Unlike the OPN family the OPM has no f-number: KC (0x28-0x2F) carries the octave in bits 6..4
	and a note code in bits 3..0, and KF (0x30-0x37) adds 1/64 of a semitone in its top six bits.
	The note codes 3, 7, 11 and 15 are unused and repeat the previous semitone, so code minus
	code >> 2 counts the semitones inside the octave. Code 0 is C#, which is what makes KC 0x4A
	(block 4, code 10) A4 - the value the codes were laid out for at 3.579545 MHz. An overclocked
	OPM plays sharp in proportion, so the clock is part of the table.

	The key register 0x08 carries the channel in bits 2..0 and an operator mask in bits 6..3; a
	zero mask is the key-off. As in the old inline path, any non-zero mask counts as a note: an
	OPM patch that keys only modulators is silent in hardware, but tracking the algorithm here
	would be a behaviour change, not a refactor. */
class OpmDecoder final : public IChipDecoder
{
public:
	OpmDecoder();
	~OpmDecoder() override;

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	int channelCount() const override;
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	ChannelKind channelKind (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace toybox::chips
