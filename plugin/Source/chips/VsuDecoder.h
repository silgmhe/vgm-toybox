#pragma once

#include <cstdint>
#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Virtual Boy's VSU: five wavetable voices and one noise voice.

	The pitch is a register, and the arithmetic closes on the sample rate rather than on a measured
	cycle. The core runs at `smplrate = clock / 120` (vsu.c:634), decrements `FreqCounter` by the
	length of each output sample in master clocks - `RC_SET_RATIO(cycleCntr, clock, smplrate)`, which
	is 120 - and reloads it with `2048 - EffFreq`. So the counter counts master clocks, and

		steps per second = clock / (2048 - freq)
		frequency        = clock / (32 * (2048 - freq))

	with thirty-two steps to a wave cycle (`WavePos` in [0, 0x20)).

	The address space is one block of sixteen bytes per voice, `voice = offset >> 4`:

	  0x0  IntlControl: bit 7 is the key - writing it with 7 set starts the voice
	  0x1  IntlControl high half (interval)
	  0x2  frequency, low byte
	  0x3  frequency, high three bits
	  0x4  envelope control
	  0x5  waveform base address - the shape, not the note
	  0x6..0xF  modulation and sweep, which change the note but are not where it comes from

	The sixth voice (offset 0x50) is an LFSR noise source: it has a frequency register, but what it
	produces is noise, so it reports an unpitched trigger rather than a note. */
class VsuDecoder final : public IChipDecoder
{
public:
	VsuDecoder();
	~VsuDecoder() override;

	VsuDecoder (const VsuDecoder&) = delete;
	VsuDecoder& operator= (const VsuDecoder&) = delete;

	void reset (std::uint32_t clock, double sampleRate) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	int channelCount() const override { return 6; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	Family family (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace toybox::chips
