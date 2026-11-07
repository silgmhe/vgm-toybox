#pragma once

#include <cstdint>
#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Atari Lynx's Mikey: four channels, each a timer and a shift register.

	**It has no wave table.** What a channel emits is the recurrence in `MikeyPattern.h` - a square
	whose polarity comes from a twelve-bit LFSR clocked by the timer - so the shape is a property of
	(seed, taps, volume, integrate) rather than bytes the song uploaded. Two consequences:

	- The row can draw a real cycle, but its length is the **LFSR period**: two steps for the
	  alternating pattern a tone uses, seven for the taps these drivers pick, hundreds for noise.
	  When the period is past `kMaxWavePoints` there is no shape to draw and the row says so.
	- `hz` follows the period: `clock / ((backup + 1) * 2^(shift + 4))` is the **step rate**, and the
	  fundamental is that divided by the number of steps in the cycle. A noise channel therefore has
	  a very low "pitch" instead of a plausible-looking lie.

	The timer side, which the pitch comes from (`mikey.c:355`, `type` = `1 << (shift + 4)`):

		stepRate = clock / ((backup + 1) * 2^(shift + 4))

	Registers, eight per channel at `0x20 + ch * 8` (`mikey.c:790-846`):

	  0 VOLCNTRL  signed eight-bit level
	  1 FEEDBACK  tap selector, bits 0-5, plus bits 6-7 shifted up to 7-8
	  2 OUTPUT    written directly - a PCM channel would use this; none of the songs here do
	  3 SHIFT     low eight bits of the register
	  4 BACKUP    the timer's reload value
	  5 CTRLA     bits 0-2 the clock shift, bit 5 integrate, bit 7 an extra tap
	  6 COUNTER   the live count, not a configuration
	  7 OTHER     control B in the low nibble, the register's top four bits in the high one

	`0x20` in **CTRLA** is the integrate flag (`AC_ENABLE_INTEGRATE`, `mikey.c:372`); it is not a
	bit of the volume register, which is a plain signed byte. */
class MikeyDecoder final : public IChipDecoder
{
public:
	MikeyDecoder();
	~MikeyDecoder() override;

	MikeyDecoder (const MikeyDecoder&) = delete;
	MikeyDecoder& operator= (const MikeyDecoder&) = delete;

	/** What this chip needs besides its register writes: the clock to assume when the song names
		none. No memory: the shape is generated, not read. */
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
