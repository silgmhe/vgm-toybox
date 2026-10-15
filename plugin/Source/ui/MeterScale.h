#pragma once

#include <cmath>

#include <juce_core/juce_core.h>

namespace vgmplay::ui::meter
{

/**
	The dB scale shared by the level meters, their graduations and every gain control.

	The fader travel and the meter height both map dB **linearly** from -60 dB (bottom) to
	+6 dB (top), so a fader cap lines up with the tick marks next to it and with the level the
	channel produces: 0 dB sits at 60/66 of the travel, and the bottom of the travel is
	inaudible (-60 dB) rather than a hard zero.
*/
inline constexpr float minDb = -60.0f;
inline constexpr float maxDb = 6.0f;

/** Where a dB value sits on the scale (0 = bottom, 1 = top). */
inline float dbToPosition (float db)
{
	return juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
}

/** The dB value at a position on the scale. */
inline float positionToDb (float position)
{
	return minDb + juce::jlimit (0.0f, 1.0f, position) * (maxDb - minDb);
}

/** Where a linear level (a meter peak, or a linear gain) sits on the scale. */
inline float levelToPosition (float level)
{
	return (level > 0.0f) ? dbToPosition (20.0f * std::log10 (level)) : 0.0f;
}

/** The linear gain at a position on the scale - the curve of every fader. */
inline float positionToLevel (float position)
{
	return std::pow (10.0f, positionToDb (position) / 20.0f);
}

/** The range of a gain control (faders, the master volume): the value stays a linear gain,
	only the travel is the dB scale above. */
inline juce::NormalisableRange<double> gainRange (double minGain = 0.0, double maxGain = 2.0)
{
	return { minGain, maxGain,
			 [] (double, double, double normalised) { return (double) positionToLevel ((float) normalised); },
			 [] (double, double, double value) { return (double) levelToPosition ((float) value); } };
}

/** The graduations drawn in the scale column of a strip: the dB value and the tiny label
	(the minus signs are dropped - everything below 0 on a meter is a negative level). */
inline constexpr float tickDbs[] = { 0.0f, -6.0f, -12.0f, -24.0f, -48.0f };
inline constexpr const char* tickLabels[] = { "0", "6", "12", "24", "48" };
inline constexpr int tickCount = (int) (sizeof (tickDbs) / sizeof (tickDbs[0]));

} // namespace vgmplay::ui::meter
