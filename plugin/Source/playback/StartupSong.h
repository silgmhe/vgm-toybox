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

/**
	Splits a command line into arguments, honouring double quotes.

	`JUCEApplication::anotherInstanceStarted()` hands over the *command line string* rather than
	the array above: on Windows it is the raw tail of the second process's command line (a path
	with spaces keeps the quotes Explorer put around it), on macOS JUCE joins one or more quoted
	paths with spaces. Splitting on whitespace alone would break exactly the songs this file
	exists for - the ones with a space in their name - so the rule lives here, where it can be
	tested, instead of in the application class.

	A quote is a delimiter, never part of a value. An unterminated quote simply runs to the end,
	which is what every platform's own parser does with a malformed line.
*/
inline juce::StringArray parseCommandLine (const juce::String& commandLine)
{
	juce::StringArray arguments;
	juce::String current;
	bool inQuotes = false;

	for (const auto character : commandLine)
	{
		if (character == '"')
		{
			inQuotes = ! inQuotes;
			continue;
		}

		if (! inQuotes && juce::CharacterFunctions::isWhitespace (character))
		{
			if (current.isNotEmpty())
			{
				arguments.add (current);
				current.clear();
			}

			continue;
		}

		current += character;
	}

	if (current.isNotEmpty())
	{
		arguments.add (current);
	}

	return arguments;
}

/** The song a second instance asked for: the startup rule, applied to a command line string. */
inline juce::File chooseSongFromInstance (const juce::String& commandLine,
										  const std::function<bool (const juce::File&)>& canOpen)
{
	return chooseStartupSong (parseCommandLine (commandLine), canOpen);
}

} // namespace toybox::playback
