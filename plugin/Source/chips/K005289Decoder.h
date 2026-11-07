#pragma once

#include <cstdint>
#include <memory>
#include <vector>

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

	**The PROM is the timbre, and it is a RAM block.** The chip reads `prom[ch * 0x100 +
	waveform * 0x20 + step]`, a **four-bit** value with `- 8` applied on the way out (k005289.c:195-198),
	so the image is 512 bytes: two channels of eight waveforms of thirty-two steps. VGM carries it as
	block `0xC3`, which sits in libvgm's **RAM** table with **16-bit** addressing - the same shape as
	the ES5503's `0xE1` but with a narrower address, which is the one detail that differs between the
	two and the reason both fields are named here.

	A voice sounds with or without a PROM, so the pitch stays `registerExact`; what the image adds is
	the shape a row draws and a file exports. */
class K005289Decoder final : public IChipDecoder
{
public:
	K005289Decoder();
	~K005289Decoder() override;

	K005289Decoder (const K005289Decoder&) = delete;
	K005289Decoder& operator= (const K005289Decoder&) = delete;

	/** What this chip needs besides its register writes: the clock to assume when the song names
		none, and the PROM its timbre lives in.

		`0xC3` is a **RAM** block - the payload starts with a sixteen-bit address and no size - so
		the fifth field is `true` and the reader keeps the address in `ramOffset`. The image is
		exactly `PROM_SIZE`: `2 * 0x100` (k005289.c:59).
	*/
	static constexpr ChipRequirements kRequirements { 3579545u, true, 0xC3, 0x200, true, false };

	void attachMemory (const ChipMemory& memory) override
	{
		if (memory.bytes != nullptr && ! memory.bytes->empty())
		{
			setProm (*memory.bytes);
		}
	}

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (uint32_t sampleTime) override;

	/** Hands the chip its 512-byte PROM. Without it a voice still sounds and still reports its
		pitch - the pitch is arithmetic - but there is no waveform to draw or to export, so the row
		stays a trigger. The image survives a `reset()`: it belongs to the song, not to the run. */
	void setProm (std::vector<uint8_t> prom);

	int channelCount() const override { return 2; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	ChannelKind channelKind (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace toybox::chips
