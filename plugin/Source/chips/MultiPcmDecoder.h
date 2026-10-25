#pragma once

#include <cstdint>
#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Yamaha YMW-258-F ("GEW8", Sega 315-5560): twenty-eight sampled voices.

	Eight registers per voice, and the map is written at the top of the core (multipcm.c:9-18):

	  0x0  pan
	  0x1  sample index into the ROM's metadata table
	  0x2  pitch, low byte
	  0x3  pitch, high byte: a four-bit signed octave above a ten-bit pitch
	  0x4  voice control - bit 7 is key on, cleared is key off (the only gate this chip has)
	  0x5  bit 0 picks direct volume, bits 1-7 are attenuation (0 loudest, 0x7F quietest)
	  0x6  LFO frequency and pitch depth
	  0x7  amplitude LFO size

	The pitch arithmetic is unusual and completely linear: the core builds `freq_step_table[i] = rate
	* (1024 + i) / 1024` (multipcm.c:671) and multiplies by `2^octave` (multipcm.c:314-324). So the
	register pair is a **ratio**, not a frequency:

		ratio = (1024 + pitch) / 1024 * 2^octave

	and the note is that ratio against unity, which this decoder places at C4. The absolute octave
	would need the sample's own rate from the ROM metadata, exactly as it does for the C140 - which is
	why this is `registerExact` for the interval and a documented constant for the octave. */
class MultiPcmDecoder final : public IChipDecoder
{
public:
	MultiPcmDecoder();
	~MultiPcmDecoder() override;

	MultiPcmDecoder (const MultiPcmDecoder&) = delete;
	MultiPcmDecoder& operator= (const MultiPcmDecoder&) = delete;

	/** What this chip needs besides its register writes: the clock to assume when the song names
		none, and (for a sampled chip) the memory image its pitch is measured against. The assembler
		asks instead of carrying a table of these. */
	static constexpr ChipRequirements kRequirements { 8000000u };

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (uint32_t sampleTime) override;

	int channelCount() const override { return 28; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	ChannelKind channelKind (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace toybox::chips
