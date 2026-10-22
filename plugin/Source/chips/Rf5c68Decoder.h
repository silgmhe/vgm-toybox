#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Ricoh RF5C68 and RF5C164: eight PCM channels playing 8-bit samples out of a 64 KB RAM buffer -
	the Sega CD's sound chip, and the Mega CD's "PCM" in every Sonic CD track.

	**This is a sampled player, so section 14 of the design doc applies**: the register file says how
	fast to walk the RAM, and only the sample's own bytes say what pitch that is. The registers are

	| offset | meaning |
	|---:|---|
	| `0x00` | envelope (a level, 0..255) |
	| `0x01` | pan: low nibble left, high nibble right |
	| `0x02` / `0x03` | step, low byte then high - the address advance per chip sample, in 1/2048 byte |
	| `0x04` / `0x05` | loop start, in 256-byte units (the address a `0xFF` byte jumps back to) |
	| `0x06` | start address, in 256-byte units |
	| `0x07` | bit 7 master output, bit 6 selects the channel for `0x00`-`0x06` (`value & 7`) |
	| `0x08` | channel mask, **a cleared bit enables the channel** |

	The chip runs at `clock / 384` samples a second (`rf5c68.c`, `INIT_DEVINF`), one byte every
	`2048 / step` of them, so the byte rate is `step * (clock/384) / 2048`. Step 2048 is therefore the
	one-byte-per-sample reference the transposition is measured from - the register's own unity, not a
	recording rate, because this chip names none (§14.3, B class). The absolute octave comes from the
	sample's measured cycle when the user turns that on.

	A sample ends at a `0xFF` data byte, where the core jumps to the loop address; if that byte is also
	`0xFF` the voice dies. So a one-shot's length is exact (`start` to the first `0xFF`) while a looped
	one runs until the driver clears its channel bit.

	The sample data arrives as a **PCM bank** (`0x67` blocks of type `0x00`-`0x3F`, no ROM header) plus
	`0x68` writes; `setRam()` takes the assembled image. Without it no cycle can be measured and every
	voice reports the C4 trigger, which is what the chip itself sounds like when its RAM is empty. */
class Rf5c68Decoder final : public IChipDecoder
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

	Rf5c68Decoder();
	~Rf5c68Decoder() override;

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	/** The chip's 64 KB of sample RAM - the song's PCM bank assembled in order. Without it the pitch
		path has nothing to measure, so every voice stays the C4 trigger. Kept across `reset()`. */
	void setRam (std::vector<std::uint8_t> ram);

	int channelCount() const override { return channelCount_; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	Family family (int channel) const override;
	bool isDrum (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	int channelCount_ = (int) Rf5c68Decoder::numChannels;
};

} // namespace toybox::chips
