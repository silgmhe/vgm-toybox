#pragma once

#include <cstdint>
#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Konami K005289: two wavetable voices reading a 32-step PROM.

	Its pitch is arithmetic, so unlike the sampled players there is nothing to measure. The core
	runs at the chip clock itself (`info->rate = info->clock`, k005289.c:151), its mixer decrements
	a counter once per clock and advances the wavetable whenever it passes zero (k005289.c:190), and
	a cycle is thirty-two steps (`addr & 0x1F`, k005289.c:191):

		frequency = clock / (32 * (freq + 1))

	The register arrives inverted - the core stores `0xFFF - value` - so a larger register value is
	a *higher* note.

	Three register pairs, one per voice (`address & 1` picks the voice):

	  0x00/0x01  control: volume in the low nibble, waveform number in bits 5-7
	  0x02/0x03  LD, latch the pitch as 0xFFF - value
	  0x04/0x05  TG, the trigger that makes the latched pitch audible - the key-on edge

	The PROM decides the timbre only. A voice sounds with or without one, which is why this decoder
	has no ROM hook and no root layer: the pitch is `registerExact`. */
class K005289Decoder final : public IChipDecoder
{
public:
	K005289Decoder();
	~K005289Decoder() override;

	K005289Decoder (const K005289Decoder&) = delete;
	K005289Decoder& operator= (const K005289Decoder&) = delete;

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	int channelCount() const override { return 2; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	ChannelKind channelKind (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace toybox::chips
