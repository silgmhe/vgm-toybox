#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** Capcom's QSound (DL-1425): sixteen 8-bit PCM voices with a per-voice echo send, plus three
	4-bit ADPCM voices that the CPS1/2 drivers never used - the sound of every CPS2 board and of the
	later CPS1 games, Street Fighter II among them.

	Fifth of the sampled players, and by far the largest: sixteen voices, a 16-bit sample address
	with a 16-bit fractional phase, and a loop described by a *length* rather than an address.

	The pitch is `rate / 4096` bytes per output sample (`qsound_ctr.c`, `pcm_update()`: the rate is
	4.12 fixed point added to the 28-bit position), against an output rate of `clock / 2 / 1248`; so
	a voice advances by `rate * clock / 10223616` bytes a second. The VGM header stores the
	old 4 MHz clock and libvgm multiplies it by 15, so the chip's real clock is 60 MHz and its output
	rate 24.04 kHz - the decoder takes either form and normalises the same way. The note is that rate
	over the sample's own cycle length, measured through `chips::estimateCycleLength()`.

	Three things about the register map catch people out, and each is a test:

	- **A bank register belongs to the next voice.** Offset `(n<<3)+0` writes `voice[(n+1)%16].bank`
	  (`init_register_map()` in the core), because the address is 64 KB per bank and the driver sets
	  the bank once for the voice that follows.
	- **Nothing is a key bit.** A voice sounds while its rate and volume are both non-zero; the
	  drivers key a note by writing the rate from zero, and stop it by writing the rate back to zero
	  or the volume to zero. The decoder therefore keys on that *rising edge* and gates on the same
	  condition, which is what the core's mixer actually plays. An address write while a voice is
	  already sounding retriggers it, and `keyOns` reports both cases.
	- **The loop is a length, not an address.** The core wraps with `new_phase -= loop_len << 12`
	  when the position reaches `end`, so the loop runs from `end - loop_len` to `end`; a zero
	  `loop_len` means the sample does not loop and the note is over at `end`.

	Pan is not silence: every pan value puts the voice on one side or the other, so only the volume
	decides whether a note sounds. The ADPCM voices are separate: fixed 8 kHz, started by a write to
	`0xD6+n`, ended by their own end address, and decoded from their deltas before anything can be
	measured.

	**Percussion.** Street Fighter II's drivers keep a drum kit on a few voices - several starts of
	several short samples on one channel - and a drum has no cycle to measure. As on the K053260, a
	sample whose period does not stand out, or whose measured pitch lands above C8 (108, where a
	noise pattern's false correlation lands at these rates), is the C4 trigger with `isDrum()`; a
	sample with a real period keeps its note. */
class QSoundDecoder final : public IChipDecoder
{
public:
	enum Channel
	{
		pcm1 = 0,
		pcm2,
		pcm3,
		pcm4,
		pcm5,
		pcm6,
		pcm7,
		pcm8,
		pcm9,
		pcm10,
		pcm11,
		pcm12,
		pcm13,
		pcm14,
		pcm15,
		pcm16,
		adpcm1,
		adpcm2,
		adpcm3,
		numChannels
	};

	QSoundDecoder();
	~QSoundDecoder() override;

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	/** The sample ROM: the song's 0x8F data blocks assembled at their own offsets. It survives a
		`reset()`, because it belongs to the song rather than to the run. */
	void setRom (std::vector<std::uint8_t> rom);

	int channelCount() const override { return channelCount_; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	Family family (int channel) const override;
	bool isDrum (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	int channelCount_ = (int) QSoundDecoder::numChannels;
};

} // namespace toybox::chips
