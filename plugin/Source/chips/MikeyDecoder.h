#pragma once

#include <cstdint>
#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Atari Lynx's MIKEY: four audio channels, each a timer driving a square wave.

	Eight registers per channel, `channel = address >> 3`, `register = address & 7` (mikey.c):

	  0x0  VOLCNTRL - signed volume, and bit 0x20 selects the integrate (feedback) mode
	  0x1  FEEDBACK
	  0x4  BACKUP   - the timer's reload value
	  0x5  CONTROL A - bits 0-2 pick a clock shift, the rest enable counting and reloading
	  0x6  COUNTER  - the live down-counter

	The timer is clocked once every `1 << (shift + 4)` master clocks (mikey.c:355,
	`period = 1 << (mAudShift + 4)`), counts down to zero, reloads from BACKUP, and toggles the output -
	so a cycle is two borrows:

		frequency = clock / (2 * (backup + 1) * 2^(shift + 4))

	Register-exact, like the K005289: no root layer and no sample. */
class MikeyDecoder final : public IChipDecoder
{
public:
	MikeyDecoder();
	~MikeyDecoder() override;

	MikeyDecoder (const MikeyDecoder&) = delete;
	MikeyDecoder& operator= (const MikeyDecoder&) = delete;

	/** What this chip needs besides its register writes: the clock to assume when the song names
		none, and (for a sampled chip) the memory image its pitch is measured against. The assembler
		asks instead of carrying a table of these. */
	static constexpr ChipRequirements kRequirements { 16000000u };

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (uint32_t sampleTime) override;

	int channelCount() const override { return 4; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	ChannelKind channelKind (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace toybox::chips
