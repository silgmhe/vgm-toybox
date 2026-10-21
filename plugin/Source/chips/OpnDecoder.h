#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The OPN family: YM2203 (OPN), YM2608 (OPNA), YM2610/2610B (OPNB), YM2612 (OPN2).

	The FM part is the six-channel OPN core: four operators per channel, two register ports, an
	11-bit f-number and a 3-bit block. A channel is not "on" in the sense a PSG channel is - it
	sounds only while the operators that reach the output, the carriers of its current
	algorithm, have a total level below maximum, so a patch whose signal path ends in a silenced
	carrier is silent even though the key register says otherwise. Tracking the algorithm and
	the four total levels is what makes this decoder worth having: the old path treated every
	0x28 key-on as a note.

	Pitch follows the hardware relation f = FNUM * clock / (144 * 2^(20 - BLOCK)), with 144 the
	FM sample-clock divisor shared by the whole family - the same f-number therefore plays the
	same note on an 8 MHz OPNA and on a 7.67 MHz OPN2, and an overclocked chip plays sharp.

	What the variants add, and what the channel indices mean (they must match
	midiCapabilityFor() in LibvgmSongInstance.cpp):

	  - YM2203:  three FM channels, 0..2.
	  - YM2612:  six FM channels, 0..5; channel 6 is the DAC, whose register 0x2A is a stream of
	             sample bytes - rate-limited to one trigger per 50 ms so it reads as drum hits.
	  - YM2608:  six FM, 0..5; ADPCM-A on 6..11 and ADPCM-B on 12. ADPCM-A is a one-shot: the
	             control register starts a sample and the chip ends it itself, so all this decoder
	             can report is a pulse wide enough for a MIDI note. ADPCM-B has a real pitch
	             register (DELTA-N), so with its 0x82 ROM in hand it reports the sample's own
	             pitch; without the ROM, or for a sample that has no period, it stays a C4 trigger.
	  - YM2610/2610B: the same 13-channel layout. The non-B variant has only channels 1, 2, 4
	             and 5 wired up, so 0 and 3 report "not present". */
class OpnDecoder final : public IChipDecoder
{
public:
	enum class Variant
	{
		ym2203,
		ym2608,
		ym2610,
		ym2610b,
		ym2612
	};

	/** ym2612 is the default so an unspecified caller gets the six-channel-plus-DAC layout. */
	explicit OpnDecoder (Variant variant = Variant::ym2612);
	~OpnDecoder() override;

	void reset (std::uint32_t clock, double sampleRate) override;

	/** The DELTA-T ROM (VGM block 0x82). Without it ADPCM-B has no sample to measure, and the
		channel stays the C4 trigger it was before. */
	void setAdpcmBRom (std::vector<std::uint8_t> rom);
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	int channelCount() const override;
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	Family family (int channel) const override;
	bool isDrum (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace toybox::chips
