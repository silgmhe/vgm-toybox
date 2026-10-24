#include "WaveShape.h"

#include <cmath>
#include <cstdint>

namespace toybox::ui::waveform
{

namespace
{
	constexpr int pointsPerCycle = 32;

	int clampTo (int value, int low, int high)
	{
		return value < low ? low : (value > high ? high : value);
	}

	float clampTo (float value, float low, float high)
	{
		return value < low ? low : (value > high ? high : value);
	}
}

std::vector<float> cycleFor (int waveKind, float duty)
{
	std::vector<float> cycle;
	cycle.reserve (pointsPerCycle);

	switch ((Kind) waveKind)
	{
		case Kind::pulse:
		{
			// High for the duty setting, low for the rest. The width is clamped to at least one
			// step at either end: a pulse with no high or no low is a flat line, which reads as a
			// broken channel rather than as a 0 % or 100 % duty.
			const int high = clampTo ((int) std::lround ((double) duty * pointsPerCycle),
									  1, pointsPerCycle - 1);

			for (int i = 0; i < pointsPerCycle; ++i)
			{
				cycle.push_back (i < high ? 1.0f : -1.0f);
			}

			break;
		}

		case Kind::triangle:
		{
			// Thirty-two steps down and back up - the staircase the chip's counter produces, not a
			// smoothed line: the steps are information.
			for (int i = 0; i < pointsPerCycle; ++i)
			{
				const int step = i < pointsPerCycle / 2 ? i : pointsPerCycle - 1 - i;
				cycle.push_back ((float) step / (float) (pointsPerCycle / 2 - 1) * 2.0f - 1.0f);
			}

			break;
		}

		case Kind::noise:
		{
			// The same 15-bit feedback the chip uses, so the burst has the right texture rather
			// than looking like a random scribble. Deterministic: a display that changed every
			// frame would be worse than one that never changed.
			std::uint32_t shift = 1;

			for (int i = 0; i < pointsPerCycle; ++i)
			{
				const std::uint32_t bit = ((shift >> 0) ^ (shift >> 1)) & 1u;
				shift = (shift >> 1) | (bit << 14);
				cycle.push_back ((shift & 1u) ? 1.0f : -1.0f);
			}

			break;
		}

		case Kind::delta:
		{
			// One bit per step, integrated: what the DAC does with a stream of ones and zeros.
			float level = 0.0f;

			for (int i = 0; i < pointsPerCycle; ++i)
			{
				level += (i % 7 == 0) ? -0.6f : 0.2f;
				level = clampTo (level, -1.0f, 1.0f);
				cycle.push_back (level);
			}

			break;
		}

		case Kind::none:
		case Kind::stored:
		default:
			break;
	}

	return cycle;
}

} // namespace toybox::ui::waveform
