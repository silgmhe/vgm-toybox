#pragma once

#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Seta X1-010: sixteen channels, each of which plays either a 128-byte wavetable or a PCM
	sample out of the song's data blocks.

	The two modes are different instruments behind one channel, and the chip says which one it is
	in one bit (status bit 1), so this decoder answers differently for each:

	- **Waveform mode** is melodic. Pitch is a 14-bit 6.10 fixed point value (registers 2/3) that
	  the chip optionally halves (status bit 7), and one cycle is the 128 samples of the table:
	  the core steps the table `clock * pitch / 2^26` times a second, which is where the note table
	  below comes from. The envelope half of the chip (registers 0x80-0x0FFF, envelope time and
	  number) is not modelled - it shapes the volume, not the note.
	- **PCM mode** is a one-shot from the song's 1 MB sample ROM: `0x04` is the start and `0x05` the
	  end (both in 4 KB pages), and the frequency register is a playback rate rather than a pitch.
	  The gate is therefore the sample's **own duration** - the core keys the channel off at the end
	  address, and so does this decoder - rather than a fixed guess at it.

	**The row draws whichever the channel is playing.** A waveform channel carries its 128-step
	table (`tableWave` set: one cycle, exported as a shape); a PCM channel carries the ROM bytes
	between start and end (no `tableWave`: a recording, exported at its own rate). The waveform RAM
	and the envelope RAM share the chip's register space with the channel registers (0x1000-0x1FFF
	and 0x80-0xFFF); the envelope only scales volume, so it is not kept.

	Register layout (libs/libvgm/emu/cores/x1_010.c): eight registers per channel, channel `n` at
	`8 * n`. The VGM command carries a 16-bit offset, which reaches this decoder as the write's
	`port` (high byte) and `reg` (low byte). */
class X1_010Decoder final : public IChipDecoder
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
		numChannels
	};

	X1_010Decoder();
	~X1_010Decoder() override;

	/** What this chip needs besides its register writes: the clock to assume when the song names
		none, and (for a sampled chip) the memory image its pitch is measured against. The assembler
		asks instead of carrying a table of these. */
	static constexpr ChipRequirements kRequirements { 16000000u, true, 0x91, 1u << 20, false, false };

	void reset (const ChipConfig& config) override;
	void attachMemory (const ChipMemory& memory) override;
	void write (const RegisterWrite& write) override;
	void advance (uint32_t sampleTime) override;

	int channelCount() const override { return channelCount_; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	ChannelKind channelKind (int channel) const override;
	bool isDrum (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	int channelCount_ = (int) X1_010Decoder::numChannels;
};

} // namespace toybox::chips
