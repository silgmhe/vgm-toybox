#pragma once

#include <juce_core/juce_core.h>

#include <functional>

namespace toybox::playback
{

/**
	The one rule behind "double-click a song and it starts playing": take the application's
	arguments and return the first one that is an existing file the backends can open.

	Pure on purpose - arguments in, a File out, with "can this file be played" supplied as a
	predicate by the caller. The rule used to sit inside ToyboxAudioProcessor::getStartupFile where
	nothing could reach it, and the defect that made every song whose name contained a space fail
	was one line of it; no test could have caught that, because there was no way to call it.

	`arguments` must be the platform's own argv - `JUCEApplicationBase::getCommandLineParameterArray()`.
	The raw command line *string* is re-quoted for display and must never be split by hand: on
	Windows a path with spaces arrives there with backslashes in front of the quotes.
*/
inline juce::File chooseStartupSong (const juce::StringArray& arguments,
									 const std::function<bool (const juce::File&)>& canOpen)
{
	if (canOpen == nullptr)
	{
		return {};
	}

	for (const auto& argument : arguments)
	{
		const juce::File candidate (argument);
		if (candidate.existsAsFile() && canOpen (candidate))
		{
			return candidate;
		}
	}

	return {};
}

} // namespace toybox::playback
