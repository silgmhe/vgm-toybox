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

	/** **The ADPCM-A image** (VGM block `0x82`). Six voices share one ROM and each names its own
		start and end page, so a row can only draw a real shape once those bytes are here; the
		Delta-T voice streams from the *other* block (`0x83`) and stays a note-only row.

		The requirement is on the class rather than the variant because the buffer makes every OPN
		decoder from one table - a YM2612 song simply has no `0x82` block and gets an empty image.

		**Sixteen megabytes, because that is what the start page addresses.** A voice names its sample
		as `((hi << 8) | lo) << 8`, so the chip reaches 16 MB; some Neo Geo rips say so in the block
		header and place their payload above the 8 MB line (`02 The Military System` has its three
		ADPCM-A blocks at 0xA79600 and up). Sized at 8 MB, every one of those blocks was dropped for
		being out of range: the decoder then read a buffer of zeros and drew a staircase on all six
		rows while the export found no range inside the image and reported zero samples.
	*/
	static constexpr ChipRequirements kRequirements { 8000000u, true, 0x82, 16u << 20, false, false };

	void reset (const ChipConfig& config) override;
	void attachMemory (const ChipMemory& memory) override;

	/** The DELTA-T ROM (VGM block 0x82). Without it ADPCM-B has no sample to measure, and the
		channel stays the C4 trigger it was before. */
	void setAdpcmBRom (std::vector<uint8_t> rom);
	void write (const RegisterWrite& write) override;
	void advance (uint32_t sampleTime) override;

	int channelCount() const override;
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	ChannelKind channelKind (int channel) const override;
	bool isDrum (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace toybox::chips
