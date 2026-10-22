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
	- **PCM mode** is a one-shot. The frequency register is a playback rate rather than a pitch,
	  and nothing in the registers says where the sample ends, so the gate is a fixed 80 ms from
	  the key-on edge - the same marker the OPN's ADPCM-A and the RF5C68 use. A driver that leaves
	  the channel keyed on (the normal case) then still produces one note per hit.

	Register layout (libs/libvgm/emu/cores/x1_010.c): eight registers per channel, channel `n` at
	`8 * n`; offsets 0x80 and above are envelope and wavetable data and do not touch a channel. The
	VGM command carries a 16-bit offset, which reaches this decoder as the write's `port` (high
	byte) and `reg` (low byte). */
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

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	int channelCount() const override { return channelCount_; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	Family family (int channel) const override;
	bool isDrum (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	int channelCount_ = (int) X1_010Decoder::numChannels;
};

} // namespace toybox::chips
