#pragma once

#include <algorithm>
#include <cstdint>

namespace toybox::chips::fm
{

/** **How long one stage of the envelope takes**, from the chip's rate code and the note it is on.

	The chip decrements a nine-bit attenuation counter; its step counter is divided by `2^sh`, and
	`sh` comes from `eg_rate_shift[32 + 2*rate + ksr]` in `fmopn.c`. That table is not arbitrary - it
	is exactly `11 - (2*rate + ksr) / 4`, clamped at zero, which is what this computes rather than
	copying 78 bytes of it.

	**`ksr` is key scaling, and it means the same patch decays faster on a higher note.** A C6
	sweeps up to eight times the speed of a C2 on identical registers; that is the chip, not an
	approximation. This is why a note's length cannot be read from the patch alone.

	The step size (`eg_inc`) averages sixteen at the fastest rate and one at the slowest;
	`max(1, 16 >> sh)` reproduces that within a factor of two, which is enough to size a buffer.

	Returns 0 for rate 0, which means "never" rather than "instantly".
*/
inline double sweepSeconds (int rateCode, int block, int fnum, int keyScale, double sampleRate)
{
	if (rateCode <= 0 || sampleRate <= 0.0)
	{
		return 0.0;
	}

	// The chip's own key-code table: the f-number's top four bits pick one of four codes, which
	// become the low two bits of the key code alongside the block.
	static constexpr uint8_t kKeyCode[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 3, 3, 3, 3, 3 };

	const int kc = (std::clamp (block, 0, 7) << 1) | kKeyCode[(fnum >> 7) & 0x0F];
	const int ksr = std::clamp (kc >> std::clamp (3 - keyScale, 0, 3), 0, 15);
	const int sh = std::max (0, 11 - (2 * rateCode + ksr) / 4);
	const double inc = (double) std::max (1, 16 >> sh);

	// 512 attenuation units is the whole range; the counter divides by 2^sh and each step moves
	// `inc` units.
	return 512.0 * (double) (1 << sh) / inc / sampleRate;
}

/** The four stages, in seconds, for one note. */
struct NoteLength
{
	double attack = 0.0;
	double decay = 0.0;
	double sustain = 0.0;		// the part the chip does not know: it decays, it does not hold
	double release = 0.0;

	double total (void) const { return attack + decay + sustain + release; }
};

} // namespace toybox::chips::fm
