#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Konami K053260: four voices playing 8-bit PCM, 4-bit KADPCM or noise, the sound chip of
	the TMNT / Vendetta / Simpsons-era Konami boards.

	Third of the sampled players and the first one whose registers say how long a sample is. A
	voice has a 12-bit pitch, a 16-bit **length**, a 21-bit start address, a 7-bit volume and a
	3-bit pan; the chip walks the sample by `pitch / 4096` bytes per output sample at
	`clock / 64` output samples a second (k053260.c, `CLOCKS_PER_SAMPLE` and `KDSC_play`), so the
	voice advances by `pitch * clock / 262144` bytes a second - a third of the C140's and C352's
	divisor, but the same idea: **the register is a rate, not a note**, and the note is that rate
	divided by the cycle length the sample itself holds. That measurement is shared
	(`chips::estimateCycleLength()` in Pitch.h, as on the C140 and C352), and the ROM it needs comes
	from the song's `0x8E` data blocks (`setRom()`).

	Three things this chip does differently from the two Namco players, and each one is a way a
	decoder can look right and be wrong:

	- **Key-on is a rising edge in a shared register.** Offset 0x28 is a four-bit mask: a voice
	  starts when its bit goes from 0 to 1 and stops when the bit is 0 - not when a voice register
	  is written (`k053260_write`, `case 0x28`). The same register's high nibble is the per-voice
	  reverse flag, so re-writing the mask with a bit already set must not start a second note (the
	  driver does exactly that, once per voice, in the rips).
	- **The length is a register, so a note needs no gate guess.** A non-looping voice plays
	  `length` bytes from `start + 1` and stops - the core knows nothing about the song's tempo.
	  The loop bit (`0x2A` bit *i*) restarts the sample instead, so only a key-off stops it. The
	  `start + 1` is a documented quirk of the core (its comment names TMNT2 and Vendetta): the
	  first byte fetched is the one *after* the start address, and a loop wraps to `start + 0`.
	- **Two sample formats.** PCM is signed 8-bit; KADPCM is a stream of 4-bit nibbles fed through
	  a running sum (the 16-entry table in `k053260.c`), low nibble first, two nibbles per byte. A
	  KADPCM voice is decoded before it is measured, because the integrator preserves the period
	  and the raw nibbles would not.

	One more register matters for the notes: offset 0x2F bit 1 enables the chip's output at all
	(`k053260_update` returns silence without it), so a song that never sets it has no notes - the
	decoder mirrors that rather than inventing sound.

	**Percussion.** These boards put drums on the same four voices as the music, and a drum sample
	has no cycle to measure: a kick sweeps, a snare is noise. Falling back to a root rate there
	would send a *melodic* note for every hit, which a DAW then plays as a tone. So an unmeasurable
	sample is reported as a one-shot: `isDrum()` is true and the note is C4 (60), the trigger
	convention the design doc settled on, with the velocity coming from the voice's volume. A
	measured period counts as a note only when the rate turns it into a pitch someone wrote - C8 is
	the ceiling, because a drum's bytes often correlate at a handful of samples (an alternating byte
	pair repeats every two) and at this chip's rates that reads as kilohertz, not as music. Below
	the ceiling a sample keeps its real pitch, so the pitched toms on these boards still come out
	as the notes they are.

	Register map (`k053260_write`; offsets are 6-bit):

	| offset | meaning |
	|---:|---|
	| 0x02, 0x03 | sub-to-main ports, no audio |
	| 0x08 + 8n + 0/1 | voice *n* pitch: bit 0-7, then bit 8-11 |
	| 0x08 + 8n + 2/3 | voice *n* length: low, high |
	| 0x08 + 8n + 4/5/6 | voice *n* start: low, middle, high (21 bits) |
	| 0x08 + 8n + 7 | voice *n* volume, 7 bits |
	| 0x28 | key-on mask (bit *i*), reverse (bit *i* + 4) |
	| 0x2A | loop (bit *i*), KADPCM (bit *i* + 4) |
	| 0x2C, 0x2D | pan for voices 0/1 and 2/3, 3 bits each; pan 0 is silent |
	| 0x2F | control: bit 1 enables sound output |

	The sample address is 21 bits with no bank register; the core masks it with the ROM's
	power-of-two size (`rom_mask`), and so does the measurement. */
class K053260Decoder final : public IChipDecoder
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

	K053260Decoder();
	~K053260Decoder() override;

	void reset (std::uint32_t clock, double sampleRate) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	/** Hands the chip its sample ROM - the song's 0x8E data blocks assembled at their own offsets
		(MidiSequenceBuilder::assembleRom()).

		Without it nothing can be measured: every voice falls back to the pitch register against the
		chip's own rate (a pitch of 0x1000, one byte per output sample, is middle C). The ROM
		survives a `reset()`: it belongs to the song, not to the run. */
	void setRom (std::vector<std::uint8_t> rom);

	int channelCount() const override { return channelCount_; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	Family family (int channel) const override;
	bool isDrum (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	int channelCount_ = (int) K053260Decoder::numChannels;
};

} // namespace toybox::chips
