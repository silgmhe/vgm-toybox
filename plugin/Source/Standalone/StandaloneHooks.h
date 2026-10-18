#pragma once

#include <juce_core/juce_core.h>

#include <functional>

namespace toybox::standalone
{

/**
	What only the standalone application can answer.

	The editor is compiled once, into a static library that the application and every plugin
	bundle link (plugin/CMakeLists.txt, target `VGMToybox`), so it cannot tell at compile time
	which wrapper it ends up in - there is no `JUCE_STANDALONE_APPLICATION` for it to check.
	It asks this table at run time instead: StandaloneApp.cpp fills it in when it creates the
	plugin holder, and in a VST3 or AAX build nobody does, so `available()` is false and the
	Audio settings page is not created at all - a plugin has the host for all of this.

	Function pointers rather than an interface with a default implementation: there is exactly
	one application per process, it owns the holder for the whole run, and a stub that silently
	did nothing would be a page with a button that does nothing.
*/
struct Hooks
{
	/** The device the application is playing through, as one line; empty when it has none open. */
	std::function<juce::String (void)> describeAudioDevice;

	/** JUCE's Audio/MIDI settings dialog - the item the standalone window's own Options menu
		carried before the window became the system's (see createWindow in StandaloneApp.cpp). */
	std::function<void (void)> showAudioSettings;
};

/** The table above. One per binary: the static library is linked into the application and into
	each plugin separately, so the application's copy is the one its own class fills in. */
inline Hooks& hooks()
{
	static Hooks table;
	return table;
}

/** True in the standalone application, false in every plugin wrapper. */
inline bool available()
{
	return hooks().showAudioSettings != nullptr;
}

} // namespace toybox::standalone
