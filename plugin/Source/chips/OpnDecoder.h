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

	/** **How much the ADPCM-A voices can address: 16 MB.** A voice names its sample as
		`((hi << 8) | lo) << 8`, so the page number spans the whole 24-bit space, and a rip may say so
		in its block header and place the payload above the 8 MB line.

		A named constant because **two** places need it and they used to be written twice: this file's
		requirement (what the decoder receives) and the sample collector's cap (what the export scans).
		Both said 8 MB; a song whose blocks sat above that line drew a staircase from a zero-filled
		image and exported nothing - `docs/memo/2026-11-09-the-image-that-was-too-small.md`.
	*/
	static constexpr std::size_t kAdpcmAImageBytes = 16u << 20;

	/** **The Delta-T image can address 16 MB too**: its start/end registers are 16-bit pages, shifted
		by eight on the OPNB and by five on the OPNA - same reason as `kAdpcmAImageBytes`.
	*/
	static constexpr std::size_t kAdpcmBImageBytes = 16u << 20;

	/** The VGM block types an OPN takes its samples from. Named because three readers ask which is
		which: the requirements below, the replayer that serves them, and the MIDI builder, which builds
		them per device. */
	static constexpr uint8_t kAdpcmABlock = 0x82;
	static constexpr uint8_t kOpnbDeltaTBlock = 0x83;
	static constexpr uint8_t kOpnaDeltaTBlock = 0x81;

	/** **Where a variant's Delta-T ROM lives, and what to fall back to.**

		The OPNB keeps it in a block of its own (`0x83`). The OPNA numbers it `0x81`, but many rips ship
		only the ADPCM-A block because on that board the two families read the same part - hence the
		fallback. A variant with no Delta-T answers zeros.

		One table, because two readers ask: the replayer (through `kRequirements`, which names both
		types and lets whichever block the song carries arrive) and the MIDI builder, which has to build
		the image per device, where the variant is known. **It used to answer `0x82` for every variant**,
		so an OPNB's Delta-T pitch was measured from the ADPCM-A bytes - the note was off by the sample's
		root, but only with the "transpose by sample root" setting on, which is why it went unnoticed.
	*/
	struct SampleBlocks
	{
		uint8_t deltaT = 0;			// the variant's Delta-T block, 0 when it has none
		uint8_t deltaTFallback = 0;	// a second block to try (the OPNA's shared part), 0 = none
	};

	static constexpr SampleBlocks sampleBlocksFor (Variant variant)
	{
		switch (variant)
		{
		case Variant::ym2610:
		case Variant::ym2610b:
			return SampleBlocks { kOpnbDeltaTBlock, 0 };

		case Variant::ym2608:
			return SampleBlocks { kOpnaDeltaTBlock, kAdpcmABlock };

		default:
			return SampleBlocks {};
		}
	}

	/** **How far a Delta-T address register is shifted on this variant.**

		The OPNB's start/end registers count 256-byte pages (shift 8); the OPNA's count 32-byte ones
		(shift 5). The exporter needs the same rule to turn a latched pair into a byte range - a second
		copy of `8` and `5` there would export a different sample than the panel draws the first time a
		board's page size is changed. */
	static constexpr int addressShiftFor (Variant variant)
	{
		switch (variant)
		{
		case Variant::ym2610:
		case Variant::ym2610b:
			return 8;

		case Variant::ym2608:
			return 5;

		default:
			return 0;
		}
	}

	/** **The images this decoder wants.** `0x82` is the ADPCM-A ROM the six voices page through;
		Delta-T reads a block of its own, and VGM numbers it per board (`0x83` on the OPNB, `0x81` on
		the OPNA), so both types are named and the replayer hands over whichever the song carries.

		The requirement is on the class rather than the variant because the buffer makes every OPN
		decoder from one table - a YM2612 song has neither block and gets empty images.
	*/

	static constexpr ChipRequirements kRequirements { 8000000u, true, kAdpcmABlock, kAdpcmAImageBytes,
													  false, false,
													  kOpnbDeltaTBlock, kAdpcmBImageBytes,
													  kOpnaDeltaTBlock, kAdpcmBImageBytes };

	void reset (const ChipConfig& config) override;
	void attachMemory (const ChipMemory& memory) override;
	void attachSecondaryMemory (const ChipMemory& memory) override;

	/** The DELTA-T ROM (VGM block `0x83` on the OPNB, `0x81` on the OPNA, and on the OPNA the six
		ADPCM-A voices read the same part). Without it ADPCM-B has no sample to measure, no shape to
		draw, and the channel stays the C4 trigger it was before. */
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
