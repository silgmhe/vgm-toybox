#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Sega PCM: sixteen 8-bit PCM voices, the sound chip of the System 16 boards (Galaxy Force II,
	OutRun's companions, Golden Axe).

	Sixth of the sampled players. Its registers *are* the playback state - the driver writes a channel's
	current address and then clears one bit, and the chip walks from there (segapcm.c):

	| offset | meaning |
	|---:|---|
	| 8n + 2, 3 | volume left / right, seven bits each |
	| 8n + 4, 5 | loop address, bits 8..15 then 16..23 - in **256-byte units** |
	| 8n + 6 | end address, also in 256-byte units; the core compares `addr >> 16`, so the sample ends at `(reg + 1) * 256` bytes |
	| 8n + 7 | address delta: the 24-bit position is advanced by this much per output sample |
	| 0x80 + 8n + 4, 5 | current address, bits 8..15 and 16..23 (bits 0..7 stay in the chip) |
	| 0x80 + 8n + 6 | bit 0 keys the voice *off* (it plays while clear), bit 1 disables the loop, the rest is the bank |

	So the rate is `pitch * clock / 32768` bytes a second - the core renders at `clock / 128` and one
	step of the position is 1/256 of a byte - and the note is that rate divided by the cycle the sample
	itself holds, measured once per key-on from the `0x80` ROM with `estimateCycleLength()`.

	**A voice is a drum or a note, and the decision is per key-on**, because these boards put both on
	the same sixteen voices: a slap bass and a snare differ in the sample, not in the register. A
	sample that does not repeat, or repeats faster than C8, is the C4 trigger; a measured period keeps
	its real pitch. The gate is the end address (or the key-off when the voice loops).
	*/
class SegaPcmDecoder final : public IChipDecoder
{
public:
	enum Channel
	{
		channel0 = 0,
		numChannels = 16
	};

	SegaPcmDecoder();
	~SegaPcmDecoder() override;

	void reset (std::uint32_t clock, double sampleRate) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	/** The song's `0x80` data blocks assembled at their own offsets. Without it no cycle can be
		measured and every voice stays the C4 trigger. The ROM survives a `reset()`. */
	void setRom (std::vector<std::uint8_t> rom);

	int channelCount() const override { return (int) SegaPcmDecoder::numChannels; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	Family family (int channel) const override;
	bool isDrum (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace toybox::chips
