#pragma once

#include <juce_data_structures/juce_data_structures.h>

namespace toybox::settings
{

/**
	The user's preferences for the whole application, in one place.

	They live in a file of our own under the platform's application data directory - in a
	"VGMToybox" folder inside it, never in that directory's root, and never in the registry:

	    Windows   %APPDATA%\VGMToybox\settings.xml
	    macOS     ~/Library/Application Support/VGMToybox/settings.xml
	    Linux     ~/.config/VGMToybox/settings.xml

	The root comes from juce::File::getSpecialLocation (userApplicationDataDirectory), which *is*
	that directory on each platform; macOS gets the "Application Support" segment on top of it,
	which is the convention there and what juce::PropertiesFile itself insists on (its Options
	documentation says so, with a runtime assertion behind it).

	The file is read once, when this object is first used, and written only when a value changes:
	nothing stays open between the two, no lock is held, and no file work ever happens anywhere
	near the audio thread. A write that fails - a host whose sandbox forbids it, a read-only
	profile - is remembered in `lastError()` and shown in the settings window, while the value
	stays live in memory and the application carries on. Losing a preference is not a reason to
	fail a plugin load.

	The plugin and the standalone build share this one file, which is why the folder and the file
	name are constants here rather than anything derived from the running application: inside a
	host there is no application name to ask, and "the settings of VGMToybox" must not depend on
	who loaded us.

	Not a juce::DeletedAtShutdown: the only thing the destructor has to do is flush a value that
	was already written when it changed, and a plain function-local static (the pattern
	playback/BackendRegistry uses) is destroyed at the right time for that.

	See docs/desgin/settings-and-theme-design.md sections 2, 4 and 10 (ADR-2).
*/
class SettingsStore : public juce::ChangeBroadcaster
{
public:
	/** Which palette the window draws with. `followSystem` asks the desktop at the moment it is
		needed, so it keeps meaning "whichever the system is set to now" - see ui/Theme.h. */
	enum class ThemeMode { light, dark, followSystem };

	/** The one instance. Built on first use, so a build that never opens a settings window
		(and the console tests) never touches the file at all. */
	static SettingsStore& getInstance (void);

	ThemeMode getThemeMode (void) const;

	/** Stores the mode, writes it, and tells every listener (each open editor) to repaint.
		Storing the value the user already has is a no-op, so nothing is written for a click that
		changed nothing. */
	void setThemeMode (ThemeMode mode);

	/** Empty when all is well, otherwise one line for the General page: what could not be done
		and to which path. Never a reason to stop. */
	juce::String lastError (void) const;

	/** Deletes every stored value and puts the defaults back. True when the file was rewritten. */
	bool resetToDefaults (void);

	/** Reads the file again and tells listeners about it. Nothing in the application needs this -
		the store reads once, at startup, on purpose - but "the file someone broke by hand" can
		only be observed from a running process by reading again, which is what the console test
		does (the same reason ui/MainMenu.h has invoke() for the window scripts). False, with
		lastError() set, when the file cannot be read; the values already in memory stay. */
	bool reload (void);

	/** The folder the settings live in: <userApplicationDataDirectory>/VGMToybox.
		The environment variable VGMTOYBOX_SETTINGS_DIR overrides it (read once, at startup) -
		that is how the console test keeps its hands off a developer's real preferences. */
	static juce::File settingsDirectory (void);

	/** The file itself: settingsDirectory()/settings.xml. */
	static juce::File settingsFile (void);

	/** The spelling the file uses, so a hand-edited file stays readable ("light", "dark",
		"followSystem"; anything else reads back as light). */
	static juce::String toString (ThemeMode mode);
	static ThemeMode themeModeFromString (const juce::String& text);

private:
	SettingsStore (void);
	~SettingsStore (void) override;

	std::unique_ptr<juce::PropertiesFile> file;
	juce::String lastErrorMessage;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsStore)
};

} // namespace toybox::settings
