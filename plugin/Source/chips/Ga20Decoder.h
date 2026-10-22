#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Irem GA20 (Nanao GA20): four 8-bit PCM voices, the sound chip of the M72/M92 boards
	(R-Type, Image Fight, X-Multiply, Undercover Cops).

	Fourth of the sampled players, and the simplest of them - which is why its two quirks are worth
	writing down, because each one turns a correct-looking decoder into silence or a wall of C4:

	- **The address unit is 16 bytes.** The two start registers are not a byte address: offset 0
	  supplies bits 4..11 and offset 1 bits 12..19 (`ch->start = (data << 4)` and `(data << 12)` in
	  `irem_ga20_w`), so the sample starts at `((reg1 << 8) | reg0) << 4`. The end registers exist
	  but the core never reads them - what actually ends a sample is a **0x00 byte in the ROM**
	  (`irem_ga20_cache_samples`: "check for sample end marker"). There is no loop.
	- **The rate register is a divider, not a step.** The position counter reloads with `rate` on
	  every overflow of an 8-bit counter, so one byte is consumed every `256 - rate` samples of the
	  core's *own* rate - and that rate is `clock / 4` (`iremga20.c`: `INIT_DEVINF(..., cfg->clock / 4,
	  ...)`), which libvgm resamples down afterwards. The byte rate is therefore
	  `clock / 4 / (256 - rate)`: the clock *is* in it. Reading the output rate here instead (44,100)
	  made every measured pitch about twenty times too low, which put every note below MIDI note 0 -
	  and that is not a note - so every voice in every song came out as the C4 trigger.

	Everything else follows the family: a voice has a start address, a rate, a hyperbolic volume
	(`data * 256 / (data + 10)`) and one control bit (offset 6, bit 1) that keys it on and off. The
	note is the byte rate divided by the cycle the sample itself holds - measured by scanning
	512-byte windows from the sample's start and keeping the first one with a period, because these
	samples *slide* (the stage theme's bass runs from 175 bytes down to 251) and a single window
	averages that away - and the ROM it needs is the song's `0x93` data blocks assembled at their
	own offsets.

	**Percussion.** These boards put drums on the same four voices as the music, and one voice can
	play both within a song - which is fine here, because the drum/melody decision is made per
	key-on from the sample the voice latched, not per channel. A drum has no cycle to measure, so
	it is reported as a one-shot: `isDrum()` is true, the note is C4 (60) and the velocity comes from
	the volume register. A measured period counts as a note only when the rate turns it into a
	pitch below C8, the same ceiling the other sampled players use.

	Register map (`irem_ga20_w`, offset = `channel * 8 + reg`, addresses in 16-byte units):

	| reg | meaning |
	|---:|---|
	| 0, 1 | start address, low then high |
	| 2, 3 | end address - written, never read by the core |
	| 4 | rate: one byte per `256 - rate` samples of clock/4 |
	| 5 | volume: `data * 256 / (data + 10)` |
	| 6 | control: bit 1 keys the voice on, clearing it keys it off |
	| 7 | read-only status (bit 0 = playing) |
	*/
class Ga20Decoder final : public IChipDecoder
{
public:
	enum Channel
	{
		channel0 = 0,
		channel1,
		channel2,
		channel3,
		numChannels
	};

	Ga20Decoder();
	~Ga20Decoder() override;

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	/** Hands the chip its sample ROM - the song's 0x93 data blocks assembled at their own offsets.
		Without it no cycle can be measured and every voice stays the C4 trigger. The ROM survives a
		`reset()`: it belongs to the song, not to the run. */
	void setRom (std::vector<std::uint8_t> rom);

	int channelCount() const override { return (int) Ga20Decoder::numChannels; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	Family family (int channel) const override;
	bool isDrum (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace toybox::chips
