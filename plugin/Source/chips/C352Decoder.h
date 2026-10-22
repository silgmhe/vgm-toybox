#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Namco C352: thirty-two voices playing 8-bit PCM out of the song's ROM data blocks, the
	sound chip of System 22, System 23 and the Super System 23 boards.

	The second decoder of the sampled players, and the C140's closest relative: nothing about a
	voice is a pitch. The frequency register is a 16-bit playback rate - the core adds it to a
	16.16 counter every output sample and fetches a byte on the carry, at an output rate of
	`clock / 288` (c352.c, `c352_update`), so the voice advances by `frequency * clock / (288 *
	65536)` bytes a second. That is the *same* divisor as the C140's (`18874368`), and the note
	follows from the rate and the rate the ROM was recorded at, which is in no register.

	The ROM answers the second half exactly as it does on the C140: `setRom()` takes the song's
	0x92 data blocks, and at key-on the decoder measures the chosen sample's own cycle length
	(`chips::estimateCycleLength()` in Pitch.h, shared with the C140) and caches it per sample. The
	note is then `playback rate / cycle length` - the sample's recorded pitch, with no root
	assumption in between. A sample whose period does not stand out (a noise voice, a drum, no
	ROM at all) falls back to `quantisePlaybackRate` against a root that follows the clock: the
	chip's own 8-bit rate, `clock / 576` (the figure c352.c's header states), so a frequency
	register at 0x8000 plays a sample back at the rate it was recorded at and sounds middle C.
	The root is an argument rather than a baked-in constant (Pitch.h), and the settings panel can
	make it a preference (design doc section 11.1).

	Three things make this chip its own decoder rather than a C140 with other offsets:

	- **Key-on is a write of its own.** Setting `KEYON` (0x4000) in a voice's flag register does
	  nothing until offset `0x202` is written - the core walks all thirty-two voices there and
	  starts the ones whose `KEYON` bit is set, then clears the bit (`c352_w`). Key-off is the
	  same edge with `KEYOFF` (0x2000): a flag write plus the execute write, not the flag alone.
	  A decoder that keyed on the flag write would start every note one command early, and one
	  that ignored 0x202 would not start any at all.
	- **The end address ends the voice.** A non-looping voice stops when the pointer reaches
	  `wave_end`, so its length is the sample's own size divided by the rate - no gate guess. With
	  the loop bit set the pointer jumps back and only a key-off stops it. The byte at `wave_end`
	  is played - the core fetches, then checks for the end - so the region and the loop run to it
	  inclusively, which matters for the wavetable songs below: a sixteen-byte cycle is a semitone
	  away from a fifteen-byte one.

	Some of Namco's own drivers play entire songs from a **single sixteen-byte waveform**: the
	Galaga rips in `test_file/c352` upload one cycle, point `wave_end` at its last byte and loop
	from `wave_start`, and take every note from the frequency register alone. That cycle is shorter
	than any correlation window, so `cycleFor()` falls back to "a loop this short is one cycle"
	(`kSingleCycleBytes`) instead of the root rate - which would put the whole song more than three
	octaves too low. The measurement is still preferred whenever it can see a cycle, and the loop
	snapping still refines it.
	- **The registers are 16-bit words written whole.** The VGM's 0xE1 command carries a 16-bit
	  big-endian offset (bit 15 selects the second chip) and a 16-bit big-endian value, so both
	  bytes of `frequency`, `wave_end` or the volume pair arrive in one command. The reader splits
	  the value across `RegisterWrite::value` (high byte) and `value2` (low byte); the offset's
	  high byte travels in `port`, as the C140's does.

	Register layout (c352.c, `C352RegMap`): eight word registers per voice, voice `n` at offset
	`8 * n`:

	| offset | register | meaning |
	|---:|---|---|
	| +0 | `vol_f` | front volume: high byte left, low byte right |
	| +1 | `vol_r` | rear volume: high byte left, low byte right |
	| +2 | `frequency` | 16-bit playback rate |
	| +3 | `flags` | `0x4000` key on, `0x2000` key off, `0x0002` loop, `0x0008` mu-law, `0x0010` noise |
	| +4 | `wave_bank` | bits 16.. of the sample address |
	| +5 | `wave_start` | start offset inside the bank |
	| +6 | `wave_end` | end offset inside the bank |
	| +7 | `wave_loop` | where a looping voice jumps back to |

	Offset `0x200` is the control register (purpose unknown, ignored) and `0x202` the key-on /
	key-off execute write. The sample address is `(wave_bank << 16) | wave_start`, and the core
	masks it with the ROM's power-of-two size (`wave_mask`), so an address past the end wraps
	inside the ROM rather than reading past it - which is why the measurement below masks the same
	way instead of clipping.

	`wave_start` is latched at key-on (the core copies it into the pointer) but `wave_end`,
	`wave_loop` and the flags are read live on every fetch, so moving the end address mid-note
	changes the note that is already playing. The length is computed at key-on anyway, because a
	MIDI note cannot follow a moving end; a driver that moves it is changing the tail of a note
	that is already sounding, and the key-off that follows still ends it. */
class C352Decoder final : public IChipDecoder
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
		channel24,
		channel25,
		channel26,
		channel27,
		channel28,
		channel29,
		channel30,
		channel31,
		numChannels
	};

	C352Decoder();
	~C352Decoder() override;

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	/** Hands the chip's sample ROM to the decoder - the song's 0x92 data blocks assembled at their
		own offsets, one byte per address (MidiSequenceBuilder::assembleRom()).

		Without it nothing can be measured and every voice falls back to the root rate, so the
		notes are the register values against the chip's own rate rather than the samples' own
		pitch. The ROM survives a `reset()`: it belongs to the song, not to the run. */
	void setRom (std::vector<std::uint8_t> rom);

	int channelCount() const override { return channelCount_; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	Family family (int channel) const override;
	bool isDrum (int channel) const override { return false; }

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	int channelCount_ = (int) C352Decoder::numChannels;
};

} // namespace toybox::chips
