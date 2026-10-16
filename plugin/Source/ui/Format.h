#pragma once

#include <cmath>

#include <juce_core/juce_core.h>

namespace toybox::ui
{

/** "m:ss.cc" - shared by the song info card and the status line. */
inline juce::String formatTime (double seconds)
{
	if (seconds < 0.0 || ! std::isfinite (seconds))
	{
		seconds = 0.0;
	}
	const int totalCentiSeconds = (int) std::lround (seconds * 100.0);
	const int minutes = totalCentiSeconds / 6000;
	const int secs = (totalCentiSeconds / 100) % 60;
	const int cents = totalCentiSeconds % 100;

	return juce::String::formatted ("%d:%02d.%02d", minutes, secs, cents);
}

} // namespace toybox::ui
