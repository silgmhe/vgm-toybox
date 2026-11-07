#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <vector>

namespace toybox::chips::mikey
{
	/** **Mikey has no wave table: its shape is a twelve-bit shift register clocked by the timer.**

		`mikey.c:490-503` is the whole generator, and it is short enough to keep in one place:

			xorGate = taps & reg;  parity = popcount (xorGate) & 1;
			reg = ((reg << 1) & 0x0FFE) | (parity ^ 1);      // bit 0 is the new output bit
			out = integrate ? clamp (out ± volume) : ((reg & 1) ? +volume : -volume);

		So a channel is a **square wave whose polarity comes from an LFSR** - a tone when the taps
		make a short cycle, noise when they make a long one. Nothing here is a sample or a table the
		song uploaded; the pattern is a property of (seed, taps, volume, integrate) and repeats.

		`buildPattern` walks that recurrence from the seeded state until it returns to a state it has
		seen, and returns one period normalised to -1..1. Phone home nowhere else: the decoder draws
		it and the collector exports it, and a second copy of the recurrence would be a second
		answer to "what does this register write sound like".
	*/

	/** How far the walk looks for a repeat.

		**It stops at the cap plus a little slack, not at the register's full state space.** A cycle
		longer than `kMaxWavePoints` is rejected anyway, so walking three thousand more steps to
		*prove* it is long is pure cost - and this runs once per distinct register state, thousands of
		times per song. The slack covers the transient a shift register has before it enters its
		cycle, which is at most its width.
	*/
	constexpr int kMaxSteps = 576;

	/** The longest period worth calling a wave. A maximal-length twelve-bit LFSR would be 4095
		steps, which is noise, not a shape to draw - and at 100 Hz that is a forty-second file. */
	constexpr int kMaxWavePoints = 512;

	/** One period of the channel's output, values in -1..1, normalised by its own peak.

		`seed` is the shift register as the driver left it (twelve bits: low eight from SHIFT, high
		four from OTHER), `taps` the nine-bit selector (`mikey.c:420`, `442`), `volume` the signed
		eight-bit level and `integrate` whether the output accumulates instead of switching.

		Returns false when the pattern is longer than `kMaxWavePoints` or constant - both are honest
		"there is no shape here", and a one-point or four-thousand-point "wave" would be worse than
		none.

		**This runs on the load path.** The collector calls it once per distinct register state - tens
		of thousands of times for a song, because a driver rewrites volume and taps constantly - so the
		walk is bounded by `kMaxSteps` and the caller memoises by state. An unbounded walk here cost
		two seconds of load time before either guard existed.
	*/
	inline bool buildPattern (uint16_t seed, uint16_t taps, int volume, bool integrate,
							  std::vector<float>& out)
	{
		out.clear ();

		if (volume == 0)
		{
			return false;
		}

		struct State
		{
			int16_t reg;
			int8_t value;
		};

		std::map<uint32_t, int> seen;
		std::vector<int> values;
		values.reserve ((std::size_t) kMaxSteps);

		uint16_t reg = seed & 0x0FFF;
		int value = 0;
		int start = -1;
		int period = 0;

		for (int step = 0; step < kMaxSteps; ++step)
		{
			const uint32_t key = ((uint32_t) reg << 8) | (uint32_t) (value & 0xFF);
			const auto found = seen.find (key);

			if (found != seen.end())
			{
				start = found->second;
				period = step - start;
				break;
			}

			seen.emplace (key, step);

			uint32_t gate = (uint32_t) (taps & reg);
			int parity = 0;

			while (gate != 0)
			{
				parity ^= (int) (gate & 1u);
				gate >>= 1;
			}

			reg = (uint16_t) (((reg << 1) & 0x0FFE) | (uint32_t) (parity ^ 1));

			if (integrate)
			{
				value = std::clamp (value + (((reg & 1u) != 0) ? volume : -volume), -128, 127);
			}
			else
			{
				value = ((reg & 1u) != 0) ? volume : -volume;
			}

			values.push_back (value);
		}

		if (start < 0 || period < 2 || period > kMaxWavePoints)
		{
			return false;
		}

		out.reserve ((std::size_t) period);

		int peak = 0;

		for (int i = 0; i < period; ++i)
		{
			const int v = values[(std::size_t) (start + i)];
			peak = std::max (peak, std::abs (v));
			out.push_back ((float) v);
		}

		if (peak == 0)
		{
			out.clear ();
			return false;
		}

		for (auto& point : out)
		{
			point /= (float) peak;
		}

		return true;
	}
}
