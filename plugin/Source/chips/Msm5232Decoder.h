#pragma once

#include <cstdint>
#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The OKI MSM5232: eight tone generators with their own envelopes.

	The register is not a divider - it is a **key index into the chip's eighty-eight-entry ROM**
	(msm5232.c:34, "MAME's ROM table, 88 entries"). Register `0x00..0x07` is one voice each:

	  0x00..0x07  bit 7 = gate; bits 6-0 = the key index, clamped to 0x57 by the core
	              a value of 0xD8 or more selects noise instead of a pitch (msm5232.c:290)

	Eighty-eight entries is exactly the span of a piano keyboard, A0 to C8, so the index is the note:

		note = 21 + index

	which makes this chip register-exact, like the K005289: no root layer, no ROM of its own, and
	the absolute octave is decided by the chip's table rather than by a measured cycle.

	Registers 0x08..0x0F (attack/decay/release) only shape the envelope and are not read here. */
class Msm5232Decoder final : public IChipDecoder
{
public:
	Msm5232Decoder();
	~Msm5232Decoder() override;

	Msm5232Decoder (const Msm5232Decoder&) = delete;
	Msm5232Decoder& operator= (const Msm5232Decoder&) = delete;

	/** What this chip needs besides its register writes: the clock to assume when the song names
		none, and (for a sampled chip) the memory image its pitch is measured against. The assembler
		asks instead of carrying a table of these. */
	static constexpr ChipRequirements kRequirements { 2119040u };

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (uint32_t sampleTime) override;

	int channelCount() const override { return 8; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	ChannelKind channelKind (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace toybox::chips
