#pragma once

#include <cstdint>
#include <memory>
#include <vector>

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

	**The pitch is a ratio with an unusual octave origin.** `freq_step_table[i]` is
	`rate * (1024 + i) / 1024` (multipcm.c:671) and the octave arrives as a shift of `octave - 1`
	(multipcm.c:314-324), so

		playback rate = rate * (1024 + pitch) / 1024 * 2^(octave - 1)

	with **octave 1 as unity**, not octave 0. Reading the register as `2^octave` - which this decoder
	did before the ROM arrived - reports every note an octave high; the `-1` is the whole reason the
	field is documented here.

	**The ROM is a metadata table first.** The first 12 bytes of each sample index name where the
	sample lives (multipcm.c:281-298): three big-endian bytes of start (bit 22 selects 12-bit packed
	data, bits 20-21 are flags), two of loop point, and two of **the two's complement negation of the
	length in samples**. A 28-voice song therefore knows every sample's length before it plays a
	single one - unlike the GA20 and the DOC, whose samples end at a marker byte.

	**Banking.** `0xC3` is not a chip register: libvgm turns it into writes to 0x10/0x11/0x12
	(`VGMPlayer::Cmd_YMW_Bank`), and the core then remaps a sample's base address through those banks
	(multipcm.c:733-743). The loader does the same, so this decoder sees ordinary register writes.
	Without it the second half of a 2 MB image plays the first half's bytes - the K007232's bank
	lesson, arriving as a VGM command instead of a register.
*/
class MultiPcmDecoder final : public IChipDecoder
{
public:
	MultiPcmDecoder();
	~MultiPcmDecoder() override;

	MultiPcmDecoder (const MultiPcmDecoder&) = delete;
	MultiPcmDecoder& operator= (const MultiPcmDecoder&) = delete;

	/** The clock to assume when the song names none, and the ROM its samples live in. `0x89` is the
		VGM block type for this chip; the blocks declare four megabytes and merge by their offsets.
		The chip's own rate is `clock / 224` (multipcm.c:129). */
	static constexpr ChipRequirements kRequirements { 8000000u, true, 0x89, 4u << 20, false, false };

	void attachMemory (const ChipMemory& memory) override
	{
		if (memory.bytes != nullptr && ! memory.bytes->empty())
		{
			setRom (*memory.bytes);
		}
	}

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (uint32_t sampleTime) override;

	int channelCount() const override { return 28; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	ChannelKind channelKind (int channel) const override;

	/** Hands the chip its sample ROM. Without it a voice has a ratio and no pitch, and a row has no
		waveform: the metadata table is where the bytes, their length and their format live. The ROM
		survives a `reset()` - it belongs to the song, not to the run. */
	void setRom (std::vector<uint8_t> rom);

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace toybox::chips
