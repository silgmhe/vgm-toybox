#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Ensoniq ES5503 "DOC": thirty-two oscillators reading 8-bit waveforms out of the chip's
	own 128 KB of RAM, the sound of the Apple IIgs, the Mirage and the ESQ-1.

	Eighth of the sampled players, and the one that is a **wavetable** chip rather than a sample
	player: an oscillator does not play a sample at a rate, it walks a table it owns, and the note
	is the rate at which that table repeats. Three things follow, each of them a way to get every
	note wrong:

	- **The register is a frequency, but its unit moves.** The accumulator gains `freq` per sample
	  and the index is `accumulator >> (9 + resolution - wavetableSize)`, so a voice reads
	  `freq * fs / 2^(9 + res - size)` bytes a second. `fs` is the *chip's* rate,
	  `(clock / 8) / (oscillatorsEnabled + 2)` - the core divides by the number of enabled
	  oscillators plus two, and libvgm resamples the result. Enabling more oscillators therefore
	  lowers every pitch, which is why a `0xE1` write rebuilds the note tables.

	- **The note is that rate divided by the table's own cycle length.** A driver is free to upload
	  a table holding one cycle or ten: the Apple IIgs way is one, and the stage theme of Undercover
	  Cops uploads 1024-byte tables holding ten cycles of a 100-byte wave, which is what makes the
	  register's 1041 read as C5 instead of a 52 Hz rumble. The cycle is measured from RAM, exactly
	  like a sample's in the GA20 decoder, and a table that holds one cycle *cannot* show a period
	  in a window of its own length - so a table that measures nothing but loops falls back to its
	  own length, while a table that measures nothing and does not loop is a drum.

	- **There is no key bit; the halt bit is the key.** Bit 0 of the control register (offset
	  `0xA0 + n`) stops the oscillator, so a note starts on its falling edge and ends on its rising
	  edge. That is the whole gate, except for the non-looping modes: one-shot mode also ends when
	  the table runs out, and that length is a number, not a guess (`0xE1` is the only other global
	  register; it is not a register write to any oscillator).

	Register map, `n` = 0..31 (`es5503_w`):

	| offset | meaning |
	|---:|---|
	| `0x00 + n` | frequency, low byte |
	| `0x20 + n` | frequency, high byte |
	| `0x40 + n` | volume, 0..255 - linear, and the core allows more than 127 |
	| `0x60 + n` | current sample byte (read-only) |
	| `0x80 + n` | wavetable pointer, high byte (the low byte is always zero) |
	| `0xA0 + n` | control: bit 0 halt, bits 1-2 mode, bit 3 IRQ, bits 4-7 output channel |
	| `0xC0 + n` | bit 6 = pointer bank, bits 3-5 = table size, bits 0-2 = resolution |
	| `0xE1` | oscillators enabled: `(value >> 1) + 1`, 1..32 |
	| `0xE0`, `0xE2` | interrupt status / A-D converter - read-only, ignored |

	The VGM command is `0xD5 <port> <register> <value>` and the RAM arrives as `0x67` blocks of
	type `0xE1` (32-bit address, then bytes), the block type libvgm's `_VGM_RAM_CHIPS` table names.

	**Percussion.** These boards put drums on the same oscillators. A one-shot table with no
	period is a drum and reports the C4 trigger; a looping table always has a pitch, because the
	table itself repeats. The MIDI channel is the oscillator, so a driver that writes the same
	note to two oscillators (this one duplicates every voice into a pair) shows two tracks.
	*/
class Es5503Decoder final : public IChipDecoder
{
public:
	static constexpr int numChannels = 32;

	Es5503Decoder();
	~Es5503Decoder() override;

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	/** Hands the chip its 128 KB of wave RAM - the song's 0xE1 blocks assembled at their own
		addresses. Without it no table can be measured: every voice stays the C4 trigger. The RAM
		survives a `reset()`: it belongs to the song, not to the run. */
	void setRam (std::vector<std::uint8_t> ram);

	int channelCount() const override { return numChannels; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	Family family (int channel) const override;
	bool isDrum (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace toybox::chips
