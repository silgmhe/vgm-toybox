#pragma once

#include <cmath>
#include <cstdint>

namespace toybox::chips
{

/** Turns a frequency into the nearest MIDI note plus the distance from it in cents.

	Every decoder's note table ends up with a frequency - the chip's dividers differ, the reference
	pitch does not - so this is the one place that knows where A4 is. Returns -1 for a frequency
	outside the MIDI range, which the note extractor reads as "no note". */
inline int quantiseFrequency (double hz, int* finePitch)
{
	if (finePitch != nullptr)
	{
		*finePitch = 0;
	}

	if (hz <= 0.0)
	{
		return -1;
	}

	const double exact = 69.0 + 12.0 * std::log2 (hz / 440.0);
	const int rounded = (int) std::lround (exact);
	if (rounded < 0 || rounded > 127)
	{
		return -1;
	}

	if (finePitch != nullptr)
	{
		*finePitch = (std::int16_t) std::lround ((exact - (double) rounded) * 100.0);
	}

	return rounded;
}

} // namespace toybox::chips
