#include "KeyboardSettings.h"
#include "NsfSettings.h"
#include "SettingsStore.h"

#include "playback/MidiSequence.h"	// the bend range limits the engine and the page share

namespace toybox::settings
{

namespace
{
	// The folder and the file name are constants, not anything derived from the running
	// application: a host gives us no application name to ask for, and the plugin and the
	// standalone have to open the same file (docs/desgin/settings-and-theme-design.md, ADR-2).
	constexpr const char* kFolderName = "VGMToybox";
	constexpr const char* kFileName = "settings.xml";

	// What the folder is called on macOS inside ~/Library: the convention there, and what
	// juce::PropertiesFile::Options demands (it asserts when this is left empty).
	constexpr const char* kMacSubFolder = "Application Support";

	// Read once, at startup: the console test points this at a temporary directory so it cannot
	// touch a developer's real preferences, and a portable install can do the same.
	constexpr const char* kDirectoryOverrideVar = "VGMTOYBOX_SETTINGS_DIR";

	constexpr const char* kThemeModeKey = "theme.mode";
	constexpr const char* kFontModeKey = "appearance.font.mode";
	constexpr const char* kFontFamilyKey = "appearance.font.family";
	constexpr const char* kLanguageIdKey = "ui.language";
	constexpr const char* kLanguageTagKey = "ui.languageTag";
	constexpr const char* kReuseWindowKey = "standalone.reuseWindow";
	constexpr const char* kRestoreLastSongKey = "standalone.restoreLastSong";
	constexpr const char* kCloseToTrayKey = "standalone.closeToTray";
	constexpr const char* kLastSongKey = "standalone.lastSong";
	constexpr const char* kNsfDurationKey = "nsf.durationSeconds";
constexpr const char* kNsfOutputGainKey = "nsf.outputGainDb";
constexpr const char* kContentViewKey = "ui.view";
constexpr const char* kKeyboardRangeHintsKey = "keyboard.rangeHints";
constexpr const char* kKeyboardStartNoteKey = "keyboard.startNote";
constexpr const char* kKeyboardNoteHoldKey = "keyboard.noteHoldMs";
constexpr const char* kKeyboardMiddleCKey = "keyboard.middleCOctave";
constexpr const char* kMidiBendRangeKey = "midi.pitchBendRange";
	constexpr const char* kMidiBendEnabledKey = "midi.pitchBendEnabled";

// The sample's own root, added to the transposition the registers ask for. Off by default: the
// root is an inference from the sample's bytes, and a wrong one shifts a whole song by a constant
// (design doc section 15).
constexpr const char* kMidiSampleRootKey = "pitch.useSampleRoot";

	juce::String writeErrorFor (const juce::File& file)
	{
		return "could not write " + file.getFullPathName();
	}
}

bool SettingsStore::settingsDirectoryIsOverridden (void)
{
	return juce::SystemStats::getEnvironmentVariable (kDirectoryOverrideVar, {}).isNotEmpty();
}

juce::File SettingsStore::settingsDirectory (void)
{
	if (settingsDirectoryIsOverridden())
	{
		return juce::File (juce::SystemStats::getEnvironmentVariable (kDirectoryOverrideVar, {}));
	}

   #if defined (TOYBOX_TEST_BUILD)
	// A test binary must never read or write the real preferences: the value of `pitch.useSampleRoot`
	// changes the notes a test asserts, so running against the machine's file makes the suite depend
	// on whoever runs it (that is how six engine tests went red on unchanged code). Every test main
	// sets VGMTOYBOX_SETTINGS_DIR through Tests/TestSettings.h; reaching this line means one forgot,
	// and a scratch folder plus a loud line is better than a run decided by a stranger's switch.
	jassertfalse;
	std::fprintf (stderr, "a test reached the real settings folder: isolate it with "
						  "toybox::test::ScopedSettingsDirectory (Tests/TestSettings.h)\n");
	return juce::File::getSpecialLocation (juce::File::tempDirectory)
		.getChildFile (juce::String (kFolderName) + "-unset-test");
   #endif

	auto root = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);

   #if JUCE_MAC
	root = root.getChildFile (kMacSubFolder);
   #endif

	return root.getChildFile (kFolderName);
}

juce::File SettingsStore::settingsFile (void)
{
	return settingsDirectory().getChildFile (kFileName);
}

SettingsStore& SettingsStore::getInstance (void)
{
	static SettingsStore instance;
	return instance;
}

SettingsStore::SettingsStore (void)
{
	// A folder of our own, made on first use. Writing settings.xml into the root of %APPDATA%
	// would be rude, and on Linux the directory JUCE hands us is not guaranteed to exist yet
	// (juce_File.h says as much next to userApplicationDataDirectory).
	if (! settingsDirectory().createDirectory())
	{
		lastErrorMessage = "could not create " + settingsDirectory().getFullPathName();
	}

	juce::PropertiesFile::Options options;
	options.folderName = kFolderName;
	options.osxLibrarySubFolder = kMacSubFolder;
	options.commonToAllUsers = false;
	options.storageFormat = juce::PropertiesFile::storeAsXML;

	// Only Options::getDefaultFile() reads these two, and that is deliberately not what names the
	// file (ADR-1). They are filled in to build the very same path anyway - applicationName plus
	// suffix is kFileName - so a later switch to that API cannot quietly point somewhere else.
	options.applicationName = "settings";
	options.filenameSuffix = ".xml";

	// The file is named by us, not by getDefaultFile(): on Linux that one lands in ~/VGMToybox,
	// which is not where XDG says settings belong (ADR-1).
	file = std::make_unique<juce::PropertiesFile> (settingsFile(), options);

	if (! file->isValidFile())
	{
		// Unreadable or corrupt: the values below read back as their defaults, and the settings
		// window says so instead of pretending the write-ahead succeeded.
		lastErrorMessage = "could not read " + settingsFile().getFullPathName();
	}
}

SettingsStore::~SettingsStore (void)
{
	// Everything is written as it changes, so this is only a backstop. saveIfNeeded() writes
	// nothing when nothing changed, which is the normal case.
	file->saveIfNeeded();
}

SettingsStore::ThemeMode SettingsStore::getThemeMode (void) const
{
	return themeModeFromString (file->getValue (kThemeModeKey));
}

void SettingsStore::setThemeMode (ThemeMode mode)
{
	if (mode == getThemeMode())
	{
		return;
	}

	file->setValue (kThemeModeKey, toString (mode));

	// Written here, with the change, rather than on the PropertiesFile timer: "read at startup,
	// written when a setting changes" is the whole contract, and a value that is on disk the
	// moment the user picked it cannot be lost by a host that kills us without saying goodbye.
	lastErrorMessage = file->saveIfNeeded() ? juce::String() : writeErrorFor (settingsFile());

	broadcastChange();
}

SettingsStore::FontMode SettingsStore::getFontMode (void) const
{
	const auto mode = fontModeFromString (file->getValue (kFontModeKey));

	// A custom mode without a family is not a usable choice - treat it like a fresh install.
	return mode == FontMode::custom && getFontFamily().isEmpty() ? FontMode::systemUI : mode;
}

juce::String SettingsStore::getFontFamily (void) const
{
	return file->getValue (kFontFamilyKey).trim();
}

void SettingsStore::setFontSelection (FontMode mode, const juce::String& family)
{
	const auto trimmed = family.trim();
	const bool useCustom = mode == FontMode::custom && trimmed.isNotEmpty();
	const auto normalizedMode = useCustom ? FontMode::custom : FontMode::systemUI;
	const auto normalizedFamily = useCustom ? trimmed : juce::String();

	if (normalizedMode == getFontMode() && normalizedFamily == getFontFamily())
	{
		return;
	}

	file->setValue (kFontModeKey, toString (normalizedMode));

	if (useCustom)
	{
		file->setValue (kFontFamilyKey, normalizedFamily);
	}
	else
	{
		file->removeValue (kFontFamilyKey);
	}

	lastErrorMessage = file->saveIfNeeded() ? juce::String() : writeErrorFor (settingsFile());
	broadcastChange();
}

juce::String SettingsStore::getLanguageId (void) const
{
	return file->getValue (kLanguageIdKey);
}

juce::String SettingsStore::getLanguageTag (void) const
{
	return file->getValue (kLanguageTagKey);
}

void SettingsStore::setLanguageSelection (const juce::String& id, const juce::String& tag)
{
	const bool unchanged = id == file->getValue (kLanguageIdKey)
						   && tag == file->getValue (kLanguageTagKey);

	if (unchanged)
	{
		return;
	}

	if (id.isEmpty())
	{
		file->removeValue (kLanguageIdKey);
		file->removeValue (kLanguageTagKey);
	}
	else
	{
		file->setValue (kLanguageIdKey, id);
		file->setValue (kLanguageTagKey, tag);
	}

	lastErrorMessage = file->saveIfNeeded() ? juce::String() : writeErrorFor (settingsFile());
	broadcastChange();
}

void SettingsStore::notifyChanged (void)
{
	broadcastChange();
}

bool SettingsStore::getReuseWindow (void) const
{
	// An absent value - a file from 0.2.x, or a fresh install - means on: the window that is
	// already running is the one a double-clicked song should go to.
	return file->getBoolValue (kReuseWindowKey, true);
}

void SettingsStore::setReuseWindow (bool shouldReuse)
{
	if (shouldReuse == getReuseWindow())
	{
		return;
	}

	file->setValue (kReuseWindowKey, shouldReuse);

	// Written with the change, like the theme: the standalone reads this before it has a window,
	// so a value that only lived in memory until exit would be one launch late.
	lastErrorMessage = file->saveIfNeeded() ? juce::String() : writeErrorFor (settingsFile());

	broadcastChange();
}

bool SettingsStore::getRestoreLastSong (void) const
{
	// Absent - a file from before this switch existed, or a fresh install - means on: carrying on
	// where the last session stopped is what launching the application should do.
	return file->getBoolValue (kRestoreLastSongKey, true);
}

void SettingsStore::setRestoreLastSong (bool shouldRestore)
{
	if (shouldRestore == getRestoreLastSong())
	{
		return;
	}

	file->setValue (kRestoreLastSongKey, shouldRestore);
	lastErrorMessage = file->saveIfNeeded() ? juce::String() : writeErrorFor (settingsFile());

	broadcastChange();
}

bool SettingsStore::getCloseToTray (void) const
{
	// Absent - a fresh install, or a file from before this switch existed - means off: a process
	// that outlives its window must be the user's request, not something a close quietly did.
	return file->getBoolValue (kCloseToTrayKey, false);
}

void SettingsStore::setCloseToTray (bool shouldCloseToTray)
{
	if (shouldCloseToTray == getCloseToTray())
	{
		return;
	}

	file->setValue (kCloseToTrayKey, shouldCloseToTray);
	lastErrorMessage = file->saveIfNeeded() ? juce::String() : writeErrorFor (settingsFile());

	broadcastChange();
}

namespace
{
}

int SettingsStore::getNsfDurationSeconds (void) const
{
	// Three minutes by default: an NSF header has no length, so the only honest source is a number
	// the user can see and change. Clamped on read, like every other number here, so a hand-edited
	// file cannot produce a seek bar that lies.
	return juce::jlimit (toybox::settings::nsf::minDurationSeconds, toybox::settings::nsf::maxDurationSeconds,
						 file->getIntValue (kNsfDurationKey, toybox::settings::nsf::defaultDurationSeconds));
}

void SettingsStore::setNsfDurationSeconds (int seconds)
{
	const int clamped = juce::jlimit (toybox::settings::nsf::minDurationSeconds, toybox::settings::nsf::maxDurationSeconds, seconds);

	if (clamped == getNsfDurationSeconds())
	{
		return;
	}

	file->setValue (kNsfDurationKey, clamped);
	lastErrorMessage = file->saveIfNeeded() ? juce::String() : writeErrorFor (settingsFile());
}

int SettingsStore::getKeyboardMiddleCOctave (void) const
{
	const int stored = file != nullptr ? file->getIntValue (kKeyboardMiddleCKey, toybox::settings::keyboard::defaultMiddleCOctave)
									   : toybox::settings::keyboard::defaultMiddleCOctave;

	return juce::jlimit (toybox::settings::keyboard::minMiddleCOctave,
						 toybox::settings::keyboard::maxMiddleCOctave, stored);
}

void SettingsStore::setKeyboardMiddleCOctave (int octave)
{
	if (file != nullptr)
	{
		file->setValue (kKeyboardMiddleCKey,
						juce::jlimit (toybox::settings::keyboard::minMiddleCOctave,
									  toybox::settings::keyboard::maxMiddleCOctave, octave));
	}
}

bool SettingsStore::getAudioDcBlock (void) const
{
	return file != nullptr && file->getBoolValue ("audio.dcBlock", false);
}

void SettingsStore::setAudioDcBlock (bool shouldRemove)
{
	if (file != nullptr)
	{
		file->setValue ("audio.dcBlock", shouldRemove);
	}
}

int SettingsStore::getKeyboardNoteHoldMs (void) const
{
	const int stored = file != nullptr ? file->getIntValue (kKeyboardNoteHoldKey, toybox::settings::keyboard::defaultNoteHoldMs)
									   : toybox::settings::keyboard::defaultNoteHoldMs;

	return juce::jlimit (toybox::settings::keyboard::minNoteHoldMs, toybox::settings::keyboard::maxNoteHoldMs, stored);
}

void SettingsStore::setKeyboardNoteHoldMs (int milliseconds)
{
	if (file != nullptr)
	{
		file->setValue (kKeyboardNoteHoldKey,
						juce::jlimit (toybox::settings::keyboard::minNoteHoldMs,
									  toybox::settings::keyboard::maxNoteHoldMs, milliseconds));
	}
}

int SettingsStore::getKeyboardStartNote (void) const
{
	const int stored = file != nullptr ? file->getIntValue (kKeyboardStartNoteKey, -1) : -1;

	return stored >= 0 && stored <= 108 ? stored : -1;
}

void SettingsStore::setKeyboardStartNote (int note)
{
	if (file != nullptr)
	{
		file->setValue (kKeyboardStartNoteKey, note);
	}
}

bool SettingsStore::getKeyboardRangeHints (void) const
{
	return file != nullptr ? file->getBoolValue (kKeyboardRangeHintsKey, true) : true;
}

void SettingsStore::setKeyboardRangeHints (bool shouldShow)
{
	if (file != nullptr)
	{
		file->setValue (kKeyboardRangeHintsKey, shouldShow);
	}
}

int SettingsStore::getContentViewIndex (void) const
{
	// No clamp here on purpose: the number of views belongs to the UI, and a copy of it in the
	// settings layer is one more thing to forget when a fourth view is added. The reader clamps.
	return file != nullptr ? file->getIntValue (kContentViewKey, 0) : 0;
}

void SettingsStore::setContentViewIndex (int index)
{
	if (file != nullptr)
	{
		file->setValue (kContentViewKey, juce::jlimit (0, 2, index));
	}
}

int SettingsStore::getNsfOutputGainDb (void) const
{
	return file != nullptr ? juce::jlimit (toybox::settings::nsf::minOutputGainDb, toybox::settings::nsf::maxOutputGainDb, file->getIntValue (kNsfOutputGainKey, toybox::settings::nsf::defaultOutputGainDb))
						   : toybox::settings::nsf::defaultOutputGainDb;
}

void SettingsStore::setNsfOutputGainDb (int decibels)
{
	const int clamped = juce::jlimit (toybox::settings::nsf::minOutputGainDb, toybox::settings::nsf::maxOutputGainDb, decibels);

	if (file != nullptr && clamped != getNsfOutputGainDb ())
	{
		file->setValue (kNsfOutputGainKey, clamped);
	}

	broadcastChange ();
}

int SettingsStore::getMidiPitchBendRange (void) const
{
	// Absent - a fresh install, or a file from before this setting existed - means 12: the wide
	// range most software synths ship with, and the one the MIDI page recommends. Values outside
	// what the engine can encode read back clamped, so a hand-edited file cannot break playback.
	return juce::jlimit (playback::minPitchBendRange, playback::maxPitchBendRange,
						 file->getIntValue (kMidiBendRangeKey, playback::defaultPitchBendRange));
}

void SettingsStore::setMidiPitchBendRange (int semitones)
{
	const int clamped = juce::jlimit (playback::minPitchBendRange, playback::maxPitchBendRange, semitones);
	if (clamped == getMidiPitchBendRange())
	{
		return;
	}

	file->setValue (kMidiBendRangeKey, clamped);
	lastErrorMessage = file->saveIfNeeded() ? juce::String() : writeErrorFor (settingsFile());

	broadcastChange();
}

bool SettingsStore::getMidiPitchBendEnabled (void) const
{
	// On by default: a chip with fine pitch (detune, vibrato, portamento) only reaches the
	// instrument through the bend wheel, and leaving it off silently quantises every song.
	return file->getBoolValue (kMidiBendEnabledKey, true);
}

void SettingsStore::setMidiPitchBendEnabled (bool shouldSendBends)
{
	if (shouldSendBends == getMidiPitchBendEnabled())
	{
		return;
	}

	file->setValue (kMidiBendEnabledKey, shouldSendBends);
	lastErrorMessage = file->saveIfNeeded() ? juce::String() : writeErrorFor (settingsFile());

	broadcastChange();
}

bool SettingsStore::getTransposeBySampleRoot (void) const
{
	return file->getBoolValue (kMidiSampleRootKey, false);
}

void SettingsStore::setTransposeBySampleRoot (bool shouldUseRoot)
{
	if (shouldUseRoot == getTransposeBySampleRoot())
	{
		return;
	}

	file->setValue (kMidiSampleRootKey, shouldUseRoot);
	lastErrorMessage = file->saveIfNeeded() ? juce::String() : writeErrorFor (settingsFile());

	broadcastChange();
}

juce::File SettingsStore::getLastSong (void) const
{
	const auto path = file->getValue (kLastSongKey);
	return path.isNotEmpty() ? juce::File (path) : juce::File();
}

void SettingsStore::setLastSong (const juce::File& song)
{
	const auto path = song.getFullPathName();
	if (path.isEmpty() || path == file->getValue (kLastSongKey))
	{
		return;
	}

	file->setValue (kLastSongKey, path);
	lastErrorMessage = file->saveIfNeeded() ? juce::String() : writeErrorFor (settingsFile());

	// Deliberately no broadcastChange(): this is state, not a preference. The load that produced
	// it already refreshes the window, and telling every listener here would repaint over it.
}

void SettingsStore::clearLastSong (void)
{
	if (file->getValue (kLastSongKey).isEmpty())
	{
		return;
	}

	file->removeValue (kLastSongKey);
	lastErrorMessage = file->saveIfNeeded() ? juce::String() : writeErrorFor (settingsFile());
}

void SettingsStore::broadcastChange (void)
{
	// Not sendChangeMessage(): the listeners have to have run by the time the caller returns, so
	// that what a click changed is already in force when the next line of the application runs.
	// The window scripts have no message loop at all, and deferring would put the change somewhere
	// they can never reach it. The long version is in SettingsStore.h.
	sendSynchronousChangeMessage();
}

juce::String SettingsStore::lastError (void) const
{
	return lastErrorMessage;
}

bool SettingsStore::reload (void)
{
	const bool loaded = file->reload();
	lastErrorMessage = loaded ? juce::String() : "could not read " + settingsFile().getFullPathName();

	broadcastChange();
	return loaded;
}

bool SettingsStore::resetToDefaults (void)
{
	file->clear();

	// Store the defaults rather than deleting the file: it would only be recreated by the next
	// change, and an empty file reads the same way to anyone looking at it.
	file->setValue (kThemeModeKey, toString (ThemeMode::light));
	file->setValue (kFontModeKey, toString (FontMode::systemUI));
	file->setValue (kReuseWindowKey, true);
	file->setValue (kRestoreLastSongKey, true);
	file->setValue (kCloseToTrayKey, false);
	file->setValue (kMidiBendRangeKey, playback::defaultPitchBendRange);
	file->setValue (kMidiBendEnabledKey, true);

	const bool saved = file->save();
	lastErrorMessage = saved ? juce::String() : writeErrorFor (settingsFile());

	broadcastChange();
	return saved;
}

juce::String SettingsStore::toString (ThemeMode mode)
{
	switch (mode)
	{
		case ThemeMode::dark:			return "dark";
		case ThemeMode::followSystem:	return "followSystem";
		case ThemeMode::light:			break;
	}

	return "light";
}

SettingsStore::ThemeMode SettingsStore::themeModeFromString (const juce::String& text)
{
	if (text.equalsIgnoreCase ("dark"))
	{
		return ThemeMode::dark;
	}

	if (text.equalsIgnoreCase ("followSystem"))
	{
		return ThemeMode::followSystem;
	}

	// "light", an empty value (no file yet) and anything a hand-edit left behind all land here.
	return ThemeMode::light;
}

juce::String SettingsStore::toString (FontMode mode)
{
	return mode == FontMode::custom ? "custom" : "systemUI";
}

SettingsStore::FontMode SettingsStore::fontModeFromString (const juce::String& text)
{
	if (text.equalsIgnoreCase ("custom"))
	{
		return FontMode::custom;
	}

	// "systemUI", an empty value and anything unknown all mean the safe default.
	return FontMode::systemUI;
}

} // namespace toybox::settings
