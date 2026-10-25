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

std::vector<float> cycleFor (int waveKind, float duty, int shapeParameter)
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
			//
			// Seeded and then warmed up. Starting from a single set bit is the textbook seed and
			// the wrong one here: it leaves the low bits zero for fourteen shifts, so the first
			// cycle drew as a flat line with one spike in it - which is exactly what it looked
			// like, and why the row did not read as noise at all.
			std::uint32_t shift = 0x4A5u;

			for (int i = 0; i < 16; ++i)
			{
				const std::uint32_t bit = ((shift >> 0) ^ (shift >> 1)) & 1u;
				shift = (shift >> 1) | (bit << 14);
			}

			// Holding each step for longer is the whole of what the period setting does: the same
			// texture at a slower speed. Period 0 is a hiss, period 15 a rumble, and a display that
			// drew both the same would hide the one thing that differs.
			const int period = clampTo (shapeParameter, 0, 15);
			const int hold = 1 + (period * 7) / 15;

			std::uint32_t current = shift & 1u;

			for (int i = 0; i < pointsPerCycle; ++i)
			{
				if ((i % hold) == 0)
				{
					const std::uint32_t bit = ((shift >> 0) ^ (shift >> 1)) & 1u;
					shift = (shift >> 1) | (bit << 14);
					current = shift & 1u;
				}

				cycle.push_back (current ? 1.0f : -1.0f);
			}

			break;
		}

		case Kind::delta:
		{
			// One bit per step, integrated: what the DAC does with a stream of ones and zeros.
			//
			// Seeded from the sample's identity, so two samples in one song draw two different
			// marks - which is what tells a kick from a hi-hat at a glance. The previous version
			// used a fixed `i % 7` pattern, and a regular staircase is indistinguishable from the
			// VRC6 ramp drawn two rows below it.
			//
			// The level is pulled back towards the centre as it walks, and the steps shrink over
			// the cycle: without either, the walk parks against a rail and reads as a square.
			std::uint32_t state = 0x9E3779B9u ^ (std::uint32_t) shapeParameter;

			if (state == 0)
			{
				state = 1;
			}

			float level = 0.0f;

			for (int i = 0; i < pointsPerCycle; ++i)
			{
				state ^= state << 13;
				state ^= state >> 17;
				state ^= state << 5;

				const float step = (state & 1u) ? 1.0f : -1.0f;
				const float envelope = 1.0f - (float) i / (float) (pointsPerCycle - 1) * 0.72f;

				level = level * 0.90f + step * 0.62f * envelope;
				level = clampTo (level, -1.0f, 1.0f);
				cycle.push_back (level);
			}

			break;
		}

		case Kind::saw:
		{
			// A counter that climbs by one step each period and wraps at the top - the ramp the
			// VRC6 accumulates, not a smoothed line. The steps are what the chip has.
			for (int i = 0; i < pointsPerCycle; ++i)
			{
				cycle.push_back ((float) i / (float) (pointsPerCycle - 1) * 2.0f - 1.0f);
			}

			break;
		}

		case Kind::fm:
		{
			// A carrier with its phase modulated by a second operator - the outline of what a
			// two-operator voice does, not a cycle any one instrument produces. An FM patch has
			// no waveform to read, so drawing the modulator's mark on the carrier is the honest
			// sketch: it shows that the shape belongs to the patch, not to the chip.
			const double twoPi = 6.283185307179586;

			for (int i = 0; i < pointsPerCycle; ++i)
			{
				const double phase = twoPi * (double) i / (double) pointsPerCycle;
				cycle.push_back ((float) std::sin (phase + 1.4 * std::sin (2.0 * phase)) * 0.85f);
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
