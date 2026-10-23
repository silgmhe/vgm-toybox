#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Konami K054539: eight stereo voices playing 8-bit PCM, 16-bit PCM or 4-bit DPCM, with a
	reverb engine - the sound of Mystic Warriors, Violent Storm, Gaiapolis and the rest of Konami's
	early-nineties boards.

	Fourth of the sampled players, and the one whose sample tells you where it ends. The pitch is a
	24-bit step (`k054539.c`, `KDSC`-style accumulator: each output sample adds `pitch` to a 16-bit
	fraction and fetches a unit whenever it carries), against an output rate of `clock / 384`; so a
	voice advances by `pitch * clock / 25165824` units a second, where a unit is a byte (8-bit PCM),
	a 16-bit word (16-bit PCM) or a nibble (4-bit DPCM). As on the C140, C352 and K053260 the note
	is that rate divided by the cycle length of the sample, measured through
	`chips::estimateCycleLength()`.

	Three things are this chip's own, and each one is a way to be wrong:

	- **A sample ends on a marker, not on a length.** The core stops the voice when the unit it just
	  fetched is `0x80` (8-bit), `0x8000` (16-bit) or the byte `0x88` (4-bit DPCM) - with the loop
	  bit set it jumps to the loop address instead (`k054539_update`). Since the ROM is here, the
	  distance to that marker is not a guess: it is scanned at key-on, which is what gives a
	  non-looping note its exact length.
	- **The volume is an attenuation.** Register 0x03 is 0 at full scale and 0x40 at -36 dB
	  (`voltab`), the opposite of every other sampled player here, so a decoder that treats it as a
	  level gets the dynamics backwards - loud notes where the song wants silence.
	- **The start register is also the live position.** The core writes the current position back
	  into 0x0C..0x0E every output sample and resets the playback fraction when a write differs
	  from it, so the value in the stream is the sample's start and the decoder latches it at
	  key-on rather than reading it again mid-note.

	The rest of the map: key-on is a bit in the mask at 0x214, key-off a bit in the mask at 0x215
	(both indexed by channel), the type and loop bit live at 0x200 + 2n (bits 2-3 select the format,
	bit 5 reverses the sample, 0x201 bit 0 loops it), the loop address is 0x08..0x0A, the pan
	1..0x0F right / 0x10 centre / 0x11..0x1F left, and 0x22F bit 0 gates the whole chip's output the
	way the core does. Pan 0 is *not* silence here: the core maps anything unexpected to centre, so
	only the volume decides whether a note sounds.

	**Percussion.** As on the K053260 these boards share the eight voices between music and drums,
	and a drum has no cycle to measure. A sample whose period does not stand out, or whose measured
	pitch lands above C8 (108 - where a noise pattern's false correlation lands at these rates), is
	reported as the C4 trigger with `isDrum()`, and anything below the ceiling keeps its real note.

	The ROM arrives in the song's `0x8C` data blocks (`setRom()`), 24-bit addressed and masked to a
	power of two like the core does. A chip without a ROM can still be described, but not measured:
	the notes then fall back to the pitch register against the chip's own rate `clock / 384`, with
	the top of the pitch register (0x10000, one unit per output sample) as middle C. */
class K054539Decoder final : public IChipDecoder
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

	K054539Decoder();
	~K054539Decoder() override;

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	/** Hands the chip its sample ROM - the song's 0x8C data blocks assembled at their own offsets.
		It survives a `reset()`: it belongs to the song, not to the run. */
	void setRom (std::vector<std::uint8_t> rom);

	int channelCount() const override { return channelCount_; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	ChannelKind channelKind (int channel) const override;
	bool isDrum (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	int channelCount_ = (int) K054539Decoder::numChannels;
};

} // namespace toybox::chips
