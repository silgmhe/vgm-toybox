#pragma once

#include <cstdint>
#include <vector>

namespace toybox::chips::msm5232
{
	/** **The pitch ROM: eighty-eight entries, one per piano key.** `msm5232.c:35-48`.

		Each entry packs a nine-bit counter and a three-bit divider (`ROM(counter, bindiv)`). The
		counter sets the tone generator's period; the divider picks which bit of that counter the
		chip's four outputs tap. So the same counter with a lower divider is a higher note, and the
		index is the key - which is why `index + 21` is a MIDI note (A0 at index 0, and index 48 is
		A4 at 440 Hz).
	*/
	constexpr uint16_t kPitchRom[88] = {
		0x0FFA, 0x0FDE, 0x0FC3, 0x0FAA, 0x0F92, 0x0F7B, 0x0F66, 0x0F52,
		0x0F3F, 0x0F2D, 0x0F1C, 0x0F0C, 0x0EFD, 0x0DDE, 0x0DC3, 0x0DAA,
		0x0D92, 0x0D7B, 0x0D66, 0x0D52, 0x0D3F, 0x0D2D, 0x0D1C, 0x0D0C,
		0x0CFD, 0x0BDE, 0x0BC3, 0x0BAA, 0x0B92, 0x0B7B, 0x0B66, 0x0B52,
		0x0B3F, 0x0B2D, 0x0B1C, 0x0B0C, 0x0AFD, 0x09DE, 0x09C3, 0x09AA,
		0x0992, 0x097B, 0x0966, 0x0952, 0x093F, 0x092D, 0x091C, 0x090C,
		0x08FD, 0x07DE, 0x07C3, 0x07AA, 0x0792, 0x077B, 0x0766, 0x0752,
		0x073F, 0x072D, 0x071C, 0x070C, 0x06FD, 0x05DE, 0x05C3, 0x05AA,
		0x0592, 0x057B, 0x0566, 0x0552, 0x053F, 0x052D, 0x051C, 0x050C,
		0x04FD, 0x03DE, 0x03C3, 0x03AA, 0x0392, 0x037B, 0x0366, 0x0352,
		0x033F, 0x032D, 0x031C, 0x030C, 0x02FD, 0x02FD, 0x02FD, 0x0E0D,
	};

	/** Highest index the core will read - it clamps above this (`msm5232.c:297`). */
	constexpr int kLastIndex = 0x57;

	/** The divider field of an entry: which counter bit the outputs tap (`msm5232.c:310`). */
	inline int dividerOf (int index)
	{
		const int clamped = (index < 0) ? 0 : (index > kLastIndex ? kLastIndex : index);

		return (int) ((kPitchRom[clamped] >> 9) & 0x07);
	}

	/** **The voice's own shape is the sum of the taps its group enables.**

		Every output is a fifty-percent square taken from one bit of the same free-running counter:
		bit `n` toggles every `2^n` ticks, bit `n-1` every `2^(n-1)`, and so on (`msm5232.c:305-320`).
		All four are therefore phase-aligned, and a group that enables several of them emits their
		sum - a staircase, not a plain square, with one period of `2^(n+1)` ticks.

		`enabled` is the group control's low nibble (`0x0C` / `0x0D`, `msm5232.c:620-650`): bit 0 the
		16' output, bit 3 the 2'. With a single tap enabled the result really is a square, which is
		what most of these songs use at any one moment; with several, the steps are visible.

		Returns the period's levels in -4..+4 (one unit per enabled tap) and its length in ticks.
	*/
	inline int buildShape (int divider, int enabled, std::vector<float>& out)
	{
		out.clear ();

		const int n = (divider < 0) ? 0 : (divider > 7 ? 7 : divider);
		const int count = 1 << (n + 1);

		// Which counter bits the group's four outputs tap: 16' is bit n, 8' bit n-1, and so on
		// down to the 2' output at bit n-3, floored at zero (`msm5232.c:312-320`).
		int tapBits[4] = { n, n - 1, n - 2, n - 3 };

		for (int i = 0; i < 4; ++i)
		{
			tapBits[i] = (tapBits[i] < 0) ? 0 : tapBits[i];
		}

		int active = 0;

		for (int tap = 0; tap < 4; ++tap)
		{
			if ((enabled & (1 << tap)) != 0)
			{
				++active;
			}
		}

		if (active == 0)
		{
			return 0;
		}

		out.reserve ((std::size_t) count);

		for (int tick = 0; tick < count; ++tick)
		{
			int level = 0;

			for (int tap = 0; tap < 4; ++tap)
			{
				if ((enabled & (1 << tap)) == 0)
				{
					continue;
				}

				level += ((tick >> tapBits[tap]) & 1) != 0 ? 1 : -1;
			}

			out.push_back ((float) level);
		}

		return count;
	}
}
