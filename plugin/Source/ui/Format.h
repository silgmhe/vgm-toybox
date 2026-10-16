#pragma once

#include <cmath>

#include <juce_core/juce_core.h>

#include "playback/BackendRegistry.h"

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

/**
	The sentences that name the supported formats.

	Five places in the UI said "a <formats> file" in slightly different words, which meant adding a
	backend edited sentences in five files. They all derive from the registry's list - which derives
	from the backends themselves - so a new format now shows up everywhere by itself.
*/
inline juce::String formatExtensionListForHumans (void)
{
	return playback::BackendRegistry::getInstance().getExtensionListText();
}

/** The file chooser's caption and the open button's tooltip. */
inline juce::String formatOpenPrompt (void)
{
	return "Open a " + formatExtensionListForHumans() + " file";
}

/** What the drop overlay says while a file is dragged over the window. */
inline juce::String formatDropPrompt (void)
{
	return "Drop a " + formatExtensionListForHumans() + " file here";
}

/** The empty song card's hint: how to get a file in. */
inline juce::String formatLoadHint (void)
{
	return "Load a " + formatExtensionListForHumans()
		   + " file with the button above or drop one onto this window.";
}

} // namespace toybox::ui
