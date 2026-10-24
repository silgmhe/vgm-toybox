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

	The standalone has one more file in the same folder, standalone-state.xml. It is not a
	preference: JUCE's StandalonePluginHolder owns it for the audio device, window position and
	last plugin state (see Standalone/StandaloneApp.cpp). Keeping it separate from this file
	means a language or theme change cannot overwrite a large saved plugin state.

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

	/** Which typeface the window draws with.
		`systemUI` follows the operating system's UI font (Segoe UI on Windows, SF on macOS, the
		desktop's UI font on Linux); `custom` names one installed font family. A custom family
		that has disappeared since it was stored falls back to `systemUI` when it is applied. */
	enum class FontMode { systemUI, custom };

	/** The one instance. Built on first use, so a build that never opens a settings window
		(and the console tests) never touches the file at all. */
	static SettingsStore& getInstance (void);

	ThemeMode getThemeMode (void) const;

	/** Stores the mode, writes it, and tells every listener (each open editor) to repaint.
		Storing the value the user already has is a no-op, so nothing is written for a click that
		changed nothing.

		The listeners are told **before this returns** (see broadcastChange), so the palette is in
		force by the time the click that asked for it has been handled - and the window scripts,
		which have no message loop to defer anything to, see it too. */
	void setThemeMode (ThemeMode mode);

	/** The UI typeface choice. `systemUI` is the default and follows the operating system; a
		`custom` choice is the family named by getFontFamily(). */
	FontMode getFontMode (void) const;

	/** The installed font family for a `custom` choice, or an empty string for `systemUI`.
		The value is stored even if the font later disappears, so reinstalling it brings it back. */
	juce::String getFontFamily (void) const;

	/** Stores the typeface choice and tells every listener. The font itself is applied when the
		next editor is built - JUCE fonts already in components cannot be re-resolved in place,
		which is why the Appearance page asks for a reopen rather than pretending it is live. */
	void setFontSelection (FontMode mode, const juce::String& family);

	/** The selected language pack: `builtin:zh-Hans`, `external:my-file.lang`, or empty on first
		run (the language manager then follows the system language without writing a choice). */
	juce::String getLanguageId (void) const;

	/** The language tag stored with `getLanguageId()` - `zh-Hans`, `en`, ... It lets a selection
		whose file has disappeared fall back to the built-in pack for the same language instead
		of jumping straight to English. */
	juce::String getLanguageTag (void) const;

	/** Stores a language selection and tells every open editor. */
	void setLanguageSelection (const juce::String& id, const juce::String& tag);

	/** Tells listeners that something outside the store changed what they should read - today
		the Language page after a rescan replaced the pack behind the unchanged selection. Does
		not write the file; unlike the setters, it is not a request to persist a new value. */
	void notifyChanged (void);

	/** Whether a song opened from the file manager goes into the window that is already running.

		Read once, before the message loop: the standalone asks it to decide whether a second copy
		of the application may start at all (Source/Standalone/StandaloneApp.cpp), which happens
		before any window or settings page exists. Default true - "double-click a song and it
		plays" is what most people mean.

		Windows and macOS are the platforms where a song can be handed over; Linux has no such
		path (JUCE's cross-process broadcast is a no-op there), so the value is ignored and the
		setting is not shown. */
	bool getReuseWindow (void) const;

	/** Stores the switch, writes it, and tells every listener. */
	void setReuseWindow (bool shouldReuse);

	/** Whether the standalone reopens, at startup, the song it had loaded when it last quit.

		Default true: launching the application without a song is the case this exists for, and
		the window that comes up should be the one the user left. Only the standalone reads it -
		a plugin takes its song from the host's project, not from this file - and only the
		standalone writes lastSong() below, so a plugin session can never overwrite what the
		application opens next. */
	bool getRestoreLastSong (void) const;

	/** Stores the switch, writes it, and tells every listener, like the other preferences. */
	void setRestoreLastSong (bool shouldRestore);

	/** Whether the standalone keeps running in the system tray when its window is closed.

		Default false: closing the window has always quit, and a process that survives a close
		must be something the user asked for. Read when the window is closed, not when this
		object is built, so flipping the switch in the General page applies to the very next
		close. A plugin never reads the key - it has no window of its own to close. */
	bool getCloseToTray (void) const;

	/** Stores the switch, writes it, and tells every listener, like the other preferences. */
	void setCloseToTray (bool shouldCloseToTray);

	/** How far the receiving instrument bends, in semitones each way: 1..24, default 12.

		This is the one fact about pitch bend that MIDI does not report - it lives in the
		instrument, the host does not pass it on, and a wrong value bends by the wrong interval
		(50 cents sent as if the range were 2 plays as 3 semitones at 12). The engine scales every
		bend by it and sends it as RPN 0 when playback starts, so an instrument that honours RPN
		follows this preference; one that does not has to be set to the same value by hand.

		Shared by every instance through this file, like the theme: it describes the user's
		instrument, not one project's routing. */
	int getMidiPitchBendRange (void) const;

	/** How long an NSF is taken to be, in seconds. The format carries no length and its driver
		never ends, so this is what the timeline and the seek bar are built on. */
	int getNsfDurationSeconds (void) const;
	void setNsfDurationSeconds (int seconds);

	/** Output gain for the NES, in decibels. The mix table leaves a lot of headroom, so a file
		played unamplified sits well below a VGM of the same music. Applied to the NES only. */
	/** Which panel the playback page was showing: 0 mixer, 1 chip state, 2 oscilloscope. */
	int getContentViewIndex (void) const;
	void setContentViewIndex (int index);

	int getNsfOutputGainDb (void) const;
	void setNsfOutputGainDb (int decibels);

	/** Lowest key the keyboard starts at: -1 to follow the song, otherwise a MIDI note. */
	int getKeyboardStartNote (void) const;
	void setKeyboardStartNote (int note);

	/** Whether the keyboard draws an arrow when a note falls outside the octaves on screen. */
	bool getKeyboardRangeHints (void) const;
	void setKeyboardRangeHints (bool shouldShow);

	/** Which octave number MIDI 60 carries: 3 means the keyboard calls middle C a C3. */
	int getKeyboardMiddleCOctave (void) const;
	void setKeyboardMiddleCOctave (int octave);

	/** How long a key stays lit after its note stops, in milliseconds. */
	int getKeyboardNoteHoldMs (void) const;
	void setKeyboardNoteHoldMs (int milliseconds);

	/** Stores the range (clamped to what the engine can encode) and tells every listener. */
	void setMidiPitchBendRange (int semitones);

	/** Whether the chips' fine pitch becomes pitch bend at all. Off: notes keep their nearest
		semitone and no bend message is sent - for an instrument that ignores RPN and cannot be
		matched, or for a MIDI monitor that should show plain notes. Default on. */
	bool getMidiPitchBendEnabled (void) const;

	/** Stores the switch, writes it, and tells every listener, like the other preferences. */
	void setMidiPitchBendEnabled (bool shouldSendBends);

	/** Whether a sampled voice's note also carries the sample's own root, or only the transposition
		the registers ask for. Off by default (design doc section 14).

		The transposition is `1200 * log2(rate / referenceRate)` - registers plus a documented
		nominal rate - so it is always computable and its intervals are always right. The root is
		`1200 * log2(referenceRate / cycle / middleC)`, measured out of the sample's own bytes: it
		places the absolute octave, and it is the part that can be wrong. With this off, every sample
		is taken to be recorded at middle C, which is stable across songs rather than per-sample
		accurate. */
	bool getTransposeBySampleRoot (void) const;

	/** Stores the switch, writes it, and tells every listener, like the other preferences. */
	void setTransposeBySampleRoot (bool shouldUseRoot);

	/** The song the standalone loaded last, or an empty File when there is none: a fresh
		install, or the user closed the song before quitting (closeSong() clears it).

		The path is recorded the moment a load succeeds, not at exit, so a crash still leaves the
		song to reopen. A path whose file has gone is kept until the next successful load or
		close: a drive that is not mounted today may be there tomorrow, and dropping the value
		would be the one failure the user cannot see coming. */
	juce::File getLastSong (void) const;

	/** Records `song` as the one to reopen. Nothing is written when it is already the value. */
	void setLastSong (const juce::File& song);

	/** Forgets the remembered song - File > Close Song is the user saying "not this one". */
	void clearLastSong (void);

	/** Empty when all is well, otherwise one line for the General page: what could not be done
		and to which path. Never a reason to stop. */
	juce::String lastError (void) const;

	/** Deletes every stored value - including the remembered song - and puts the defaults back.
		True when the file was rewritten. */
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

	/** True when VGMTOYBOX_SETTINGS_DIR is in force. The standalone uses this to avoid
		migrating a developer's real state file into a test's temporary folder. */
	static bool settingsDirectoryIsOverridden (void);

	/** The file itself: settingsDirectory()/settings.xml. */
	static juce::File settingsFile (void);

	/** The spelling the file uses, so a hand-edited file stays readable ("light", "dark",
		"followSystem"; anything else reads back as light). */
	static juce::String toString (ThemeMode mode);
	static ThemeMode themeModeFromString (const juce::String& text);

	/** The spelling for the typeface choice ("systemUI", "custom"; anything else reads back as
		systemUI). */
	static juce::String toString (FontMode mode);
	static FontMode fontModeFromString (const juce::String& text);

private:
	SettingsStore (void);
	~SettingsStore (void) override;

	/** Tells every listener that a stored value changed, synchronously.

		juce::ChangeBroadcaster normally posts the notification and sends it on the next message
		loop callback. That is the right default for a value that changes on its own, and the wrong
		one here: a setting changes because someone clicked something, and everything that follows
		from it - the palette, the repaint, and whatever the settings window shows - has to be true
		by the time that click has been handled. A host that saves state or screenshots the window
		in the same breath would otherwise catch the old value, and the window scripts (run with no
		message loop at all, see Tests/AppHarnessTest.cpp) would never see the new one.

		Every listener here only reads the store, so none of them can re-enter a write. */
	void broadcastChange (void);

	std::unique_ptr<juce::PropertiesFile> file;
	juce::String lastErrorMessage;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsStore)
};

} // namespace toybox::settings
