#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** OKIM6295 (MSM6295): four ADPCM voices playing fixed-rate samples out of a 256 KB ROM.

	The chip has **no pitch register at all**: every voice plays at `clock / 165` (or `/132` with
	pin 7 high), and the sample's own cycle is the only thing that decides the note. So this is the
	ES5503's case, not the C140's - the note is the measured root, labelled `sampleInferred`, and a
	voice whose sample has no measurable period keeps the C4 trigger.

	The register interface is not a register file but a two-byte command port (`0x00`): a byte with
	bit 7 set selects a sample number, the next byte's bits 4-6 say which voices start it, and a
	byte with bit 7 clear and no pending command stops the voices in bits 3-6. The sample's start
	and stop are three bytes each in the ROM at `sample * 8`; the length is `2 * (stop - start + 1)`
	nibbles, which is what makes a note's end exact. */
class Okim6295Decoder final : public IChipDecoder
{
public:
	Okim6295Decoder();
	~Okim6295Decoder() override;

	Okim6295Decoder (const Okim6295Decoder&) = delete;
	Okim6295Decoder& operator= (const Okim6295Decoder&) = delete;

	/** What this chip needs besides its register writes: the clock to assume when the song names
		none, and (for a sampled chip) the memory image its pitch is measured against. The assembler
		asks instead of carrying a table of these. */
	static constexpr ChipRequirements kRequirements { 1000000u, true, 0x8B, 256u << 10, false, false };

	void attachMemory (const ChipMemory& memory) override
	{
		if (memory.bytes != nullptr && ! memory.bytes->empty())
		{
			setRom (*memory.bytes);
		}
	}

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	int channelCount() const override;
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	ChannelKind channelKind (int channel) const override;
	bool isDrum (int channel) const override;

	/** The 256 KB sample ROM (`0x8B` blocks); without it there is neither a length nor a pitch. */
	void setRom (std::vector<std::uint8_t> rom);

	struct Impl;

private:
	std::unique_ptr<Impl> impl;
};

} // namespace toybox::chips
