#pragma once

#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Namco C140: twenty-four voices playing 8-bit PCM out of the song's ROM data blocks, the
	sound chip of System 2 and System 21 boards.

	This is the first decoder of the sampled players: nothing about a voice is a pitch. The
	frequency register is a 16-bit playback rate - the core advances the sample pointer by
	`frequency * clock / 288` bytes a second (c140.c, `delta`) - and the note follows from that rate
	and the rate the ROM was recorded at, which is in no register. `quantisePlaybackRate` (Pitch.h)
	owns that conversion and the root-rate assumption behind it; this file only supplies the rate.

	Two things make the note boundaries better than the one-shot guess the X1-010's PCM mode and
	the RF5C68 need:

	- **The end address.** A voice stops when the sample pointer reaches `end - start`, so a
	  non-looping voice has a length the registers state exactly (in bytes at a known rate) and the
	  gate can be closed on the spot. That is why there is no 80 ms marker here.
	- **Key-off.** With the loop bit set the voice never reaches its end, and only a mode write
	  without bit 7 stops it - the same edge the driver uses for a release.

	Register layout (libs/libvgm/emu/cores/c140.c, `voice_registers`): sixteen registers per voice,
	voice `n` at `16 * n`, and the VGM's 0xD4 command carries the 16-bit offset big-endian, split
	across the write's `port` (high byte) and `reg` (low byte). Offsets at 0x180 and above are the
	interval timer and do not touch a voice.

	`start`, `end` and `loop` are **latched at key-on**: the core copies them into the voice only on
	the key-on edge, so rewriting them mid-note changes the next note, not the one playing. The mode
	byte is *not* latched - the core re-reads the loop and mu-law bits every sample - so a loop bit
	written after key-on takes effect at once. Both behaviours are mirrored below. */
class C140Decoder final : public IChipDecoder
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
		channel8,
		channel9,
		channel10,
		channel11,
		channel12,
		channel13,
		channel14,
		channel15,
		channel16,
		channel17,
		channel18,
		channel19,
		channel20,
		channel21,
		channel22,
		channel23,
		numChannels
	};

	C140Decoder();
	~C140Decoder() override;

	void reset (std::uint32_t clock, double sampleRate) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	int channelCount() const override { return channelCount_; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	Family family (int channel) const override;
	bool isDrum (int channel) const override { return false; }

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	int channelCount_ = (int) C140Decoder::numChannels;
};

} // namespace toybox::chips
