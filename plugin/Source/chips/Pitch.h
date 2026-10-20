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

/** Middle C: the pitch a sample player's ROM is taken to stand at when it plays back at its
	recorded rate. */
constexpr double kMiddleCHz = 261.6255653005986;

/** Turns the playback rate of a recorded sample into a note.

	A sampled voice carries no pitch in its registers, only the number of bytes a second the chip
	advances the sample pointer at. The pitch it sounds is that rate divided by the rate the ROM
	was recorded at - which is in no register either. So the mapping needs a reference: `rootRate`
	is the rate at which the ROM plays back at its own pitch, and that playback is taken to be
	middle C. Chips differ in how they derive the rate, and their drivers in what they call the
	root, so the root is an argument rather than a constant; the MIDI output settings will make it a
	preference (design doc section 11.1), which is why it is not baked into any one decoder.

	Returns -1 for a zero rate, which the note extractor reads as "silent". */
inline int quantisePlaybackRate (double rate, double rootRate, int* finePitch)
{
	if (finePitch != nullptr)
	{
		*finePitch = 0;
	}

	if (rate <= 0.0 || rootRate <= 0.0)
	{
		return -1;
	}

	return quantiseFrequency (kMiddleCHz * rate / rootRate, finePitch);
}

} // namespace toybox::chips
