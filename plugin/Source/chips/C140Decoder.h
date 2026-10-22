#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Namco C140: twenty-four voices playing 8-bit PCM out of the song's ROM data blocks, the
	sound chip of System 2 and System 21 boards.

	This is the first decoder of the sampled players: nothing about a voice is a pitch. The
	frequency register is a 16-bit playback rate - the core advances the sample pointer by
	`frequency * clock / 288` bytes a second (c140.c, `delta`) - and the note follows from that rate
	and the rate the ROM was recorded at, which is in no register.

	The ROM itself is what answers the second half: `setRom()` takes the song's 0x8D data blocks,
	and at key-on the decoder measures the chosen sample's own cycle length (autocorrelation over
	the loop region, snapped to the loop length being a whole number of cycles) and caches it per
	sample. The note is then `playback rate / cycle length` - the sample's recorded pitch, with no
	root assumption in between. A sample whose period does not stand out (a drum, noise, a mu-law
	sample, or no ROM at all) falls back to `quantisePlaybackRate` against the 8 kHz root rate.

	That measurement is a fix, not a refinement: while the ROM was unavailable every voice was
	quantised against the 8 kHz root, and the produced notes were systematically wrong wherever a
	song's samples were recorded at another rate ("Submerged City" sent bass at 59 instead of 32
	and chords at 62 instead of 56 - the register values were right, the mapping was not). The
	chain that closes it is `VgmCommandReader::dataBlocks()` (the 0x8D blocks, still in the
	image) -> `MidiSequenceBuilder::assembleRom()` (the union of the blocks at their own ROM
	offsets) -> `setRom()` here. Any one link missing puts the notes back on the root rate, so a
	future change that drops the blocks looks like "the C140 is a bit off again" rather than like
	a broken wire - hence the note.

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

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	/** Which board's address arithmetic turns a voice's bank and address into a ROM offset.

		The chip itself only ever sees eight bits of bank and sixteen of address, and each board
		wires them into the ROM differently; libvgm keeps the board in `DEV_GEN_CFG::flags` and
		takes it from the VGM header's byte 0x96 (vgmplayer.cpp, `DEVID_C140`). System 2 is the
		default and the common case, and it is the fold that matters: the low 19 bits are the
		offset and bit 21 is shifted down, so bank 0x14 with address 0x95C0 is ROM offset 0x495C0
		and *not* 0x1495C0. On a one-megabyte ROM the unfolded address is past the end, the sample
		cannot be measured, and every note of the song silently falls back to the root rate - which
		is exactly the bug the ROM was added to fix, so the fold is not optional.

		The values are libvgm's (`c140.h`); the ASIC 219 (0x02) is a different chip that libvgm
		plays through `DEVID_C219`, so this decoder never sees it. */
	enum class Banking : std::uint8_t
	{
		system2 = 0x00,
		system21 = 0x01,
		linear = 0x03,
	};

	/** The banking a VGM header's byte 0x96 names: 0x01 and 0x03 are their own mappings, and
		every other value - 0x00, the ASIC 219's 0x02, a byte that is not a header field at all -
		gets System 2, which is what libvgm's own default does. */
	static Banking bankingFromHeaderByte (std::uint8_t headerByte);

	/** Hands the chip's sample ROM to the decoder - the song's 0x8D data blocks assembled at
		their own offsets, one byte per address - together with the banking that turns a voice's
		registers into an offset into it.

		The ROM is what turns the C140 from "a rate" into "a note": at key-on the decoder measures
		the chosen sample's own cycle length and divides the playback rate by it, so the note is
		the pitch the sample was recorded at. Before this existed every voice was quantised
		against a fixed 8 kHz root rate, and a song whose samples run at a different rate came out
		an octave or a fifth off - the bass of "Submerged City" was 59 instead of 32, its chords 62
		instead of 56 - because nothing in the registers says how fast the ROM was recorded.

		Without a ROM (no 0x8D blocks, an S98 or DRO input, a caller that does not assemble one)
		the decoder keeps the root-rate behaviour, so nothing that worked before needs the ROM.

		The ROM and the banking survive a `reset()`: only the clock and the stream rate change
		there, and both belong to the song rather than to the run. */
	void setRom (std::vector<std::uint8_t> rom, Banking banking = Banking::system2);

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
