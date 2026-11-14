#include "SettingsDefaults.h"
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

	/** Off by default: it puts a number in the status line, which is not something a person who
		did not ask for it should have to look at. */
	constexpr const char* kPaintStatsKey = "diagnostics.paintStats";

	/** The export window's two remembered choices: the mode and the length. */
	constexpr const char* kExportLoopedKey = "export.looped";
	constexpr const char* kExportSecondsKey = "export.seconds";
	constexpr const char* kExportBitsKey = "export.bits";
	constexpr const char* kExportSampleSecondsKey = "export.sampleSeconds";
	constexpr const char* kExportSampleRateKey = "export.sampleRate";

	/** The Keyboard page's redraw rate. Absent means `settings::window::defaultRefreshHz`. */
	constexpr const char* kRefreshHzKey = "ui.refreshHz";
	constexpr const char* kLastSongKey = "standalone.lastSong";
	constexpr const char* kNsfDurationKey = "nsf.durationSeconds";
	constexpr const char* kDragCleanupKey = "export.dragCleanup";
	constexpr const char* kDragRetentionKey = "export.dragRetentionHours";
	constexpr const char* kNsfOutputGainKey = "nsf.outputGainDb";
	constexpr const char* kContentViewKey = "ui.view";
	constexpr const char* kKeyboardRangeHintsKey = "keyboard.rangeHints";
	constexpr const char* kKeyboardStartNoteKey = "keyboard.startNote";
	constexpr const char* kKeyboardNoteHoldKey = "keyboard.noteHoldMs";
	constexpr const char* kKeyboardMiddleCKey = "keyboard.middleCOctave";
	constexpr const char* kMidiBendRangeKey = "midi.pitchBendRange";
	constexpr const char* kMidiBendEnabledKey = "midi.pitchBendEnabled";

	// The sample's own root, added to the transposition the registers ask for. **On by default since
	// 2026-11-09**: without it a sampled voice is placed by the playback rate against an 8 kHz
	// convention, which is an octave or two away from what the song sounds like; with it the octave
	// follows the sample, and the measurement's own error is a constant the ear can forgive. The
	// switch stays because the measurement is an inference - a wrong one moves a whole song by that
	// constant (design doc section 15, memo of the same date).
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

bool SettingsStore::getPaintStats (void) const
{
	// Off unless asked for. The probe exists because "is this window repainting while nothing
	// moves" could not be answered by reading the code, and it will be asked again; but a window
	// that prints a counter into its status line without being asked to is a window with a bug.
	return file->getBoolValue (kPaintStatsKey, window::paintStats);
}

void SettingsStore::setPaintStats (bool shouldReport)
{
	if (shouldReport == getPaintStats())
	{
		return;
	}

	file->setValue (kPaintStatsKey, shouldReport);
	broadcastChange();
}

bool SettingsStore::getExportLooped (void) const
{
	return file->getBoolValue (kExportLoopedKey, exporting::looped);
}

void SettingsStore::setExportLooped (bool looped)
{
	if (looped == getExportLooped ())
	{
		return;
	}

	file->setValue (kExportLoopedKey, looped);
	broadcastChange ();
}

int SettingsStore::getExportSeconds (void) const
{
	return juce::jlimit (exporting::minExportSeconds, exporting::maxExportSeconds,
						 file->getIntValue (kExportSecondsKey, exporting::defaultExportSeconds));
}

void SettingsStore::setExportSeconds (int seconds)
{
	const int clamped = juce::jlimit (exporting::minExportSeconds, exporting::maxExportSeconds, seconds);

	if (clamped == getExportSeconds ())
	{
		return;
	}

	file->setValue (kExportSecondsKey, clamped);
	broadcastChange ();
}

int SettingsStore::getExportBits (void) const
{
	const auto stored = file->getIntValue (kExportBitsKey, exporting::defaultExportBits);

	return juce::jlimit (exporting::minExportBits, exporting::maxExportBits, stored);
}

void SettingsStore::setExportBits (int bits)
{
	const int clamped = juce::jlimit (exporting::minExportBits, exporting::maxExportBits, bits);

	if (clamped == getExportBits ())
	{
		return;
	}

	file->setValue (kExportBitsKey, clamped);
	broadcastChange ();
}

int SettingsStore::getExportSampleSeconds (void) const
{
	return juce::jlimit (exporting::minSampleSeconds, exporting::maxSampleSeconds,
						 file->getIntValue (kExportSampleSecondsKey, exporting::defaultSampleSeconds));
}

void SettingsStore::setExportSampleSeconds (int seconds)
{
	const int clamped = juce::jlimit (exporting::minSampleSeconds, exporting::maxSampleSeconds,
										 seconds);

	if (clamped == getExportSampleSeconds())
	{
		return;
	}

	file->setValue (kExportSampleSecondsKey, clamped);
	broadcastChange ();
}

int SettingsStore::getExportSampleRate (void) const
{
	const int stored = file->getIntValue (kExportSampleRateKey, exporting::defaultSampleRate);

	if (stored == exporting::sampleRateChip || stored == exporting::sampleRateHost)
	{
		return stored;
	}

	// Only the rates the page offers, so a hand-edited file cannot ask for 3 kHz and get a file
	// nothing will open at the pitch it should.
	return (stored == 44100 || stored == 48000 || stored == 96000) ? stored
																  : exporting::defaultSampleRate;
}

void SettingsStore::setExportSampleRate (int rate)
{
	const int allowed = (rate == 44100 || rate == 48000 || rate == 96000) ? rate
					   : (rate == exporting::sampleRateChip ? exporting::sampleRateChip
														   : exporting::sampleRateHost);

	if (allowed == getExportSampleRate ())
	{
		return;
	}

	file->setValue (kExportSampleRateKey, allowed);
	broadcastChange ();
}

double SettingsStore::getExportSampleRateHz (double hostRate) const
{
	const int rate = getExportSampleRate ();

	if (rate == exporting::sampleRateChip)
	{
		return 0.0;			// the writer's "at the rate the chip read the bytes"
	}

	if (rate > 0)
	{
		return (double) rate;
	}

	// No host rate means no host - a standalone with no device open yet, or a test.
	return (hostRate > 0.0) ? hostRate : 44100.0;
}

int SettingsStore::getRefreshHz (void) const
{
	return juce::jlimit (window::minRefreshHz, window::maxRefreshHz,
						 file->getIntValue (kRefreshHzKey, window::defaultRefreshHz));
}

void SettingsStore::setRefreshHz (int hz)
{
	const int clamped = juce::jlimit (window::minRefreshHz, window::maxRefreshHz, hz);

	if (clamped == getRefreshHz())
	{
		return;
	}

	file->setValue (kRefreshHzKey, clamped);
	broadcastChange();
}

bool SettingsStore::getReuseWindow (void) const
{
	// An absent value - a file from 0.2.x, or a fresh install - means on: the window that is
	// already running is the one a double-clicked song should go to.
	return file->getBoolValue (kReuseWindowKey, window::reuseWindow);
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
	return file->getBoolValue (kRestoreLastSongKey, window::restoreLastSong);
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
	return file->getBoolValue (kCloseToTrayKey, window::closeToTray);
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
	return juce::jlimit (nsf::minDurationSeconds, nsf::maxDurationSeconds,
						 file->getIntValue (kNsfDurationKey, nsf::defaultDurationSeconds));
}

void SettingsStore::setNsfDurationSeconds (int seconds)
{
	const int clamped = juce::jlimit (nsf::minDurationSeconds, nsf::maxDurationSeconds, seconds);

	if (clamped == getNsfDurationSeconds())
	{
		return;
	}

	file->setValue (kNsfDurationKey, clamped);
	lastErrorMessage = file->saveIfNeeded() ? juce::String() : writeErrorFor (settingsFile());
}

bool SettingsStore::getDragCleanupEnabled (void) const
{
	return file->getBoolValue (kDragCleanupKey, exporting::defaultCleanupEnabled);
}

void SettingsStore::setDragCleanupEnabled (bool enabled)
{
	if (enabled == getDragCleanupEnabled())
	{
		return;
	}

	file->setValue (kDragCleanupKey, enabled);
	lastErrorMessage = file->saveIfNeeded() ? juce::String() : writeErrorFor (settingsFile());
}

int SettingsStore::getDragRetentionHours (void) const
{
	// Clamped on read like every other number here: a hand-edited file must not be able to ask
	// the cleaner for "delete everything" or for a retention measured in years.
	return juce::jlimit (exporting::minRetentionHours,
						 exporting::maxRetentionHours,
						 file->getIntValue (kDragRetentionKey,
											exporting::defaultRetentionHours));
}

void SettingsStore::setDragRetentionHours (int hours)
{
	const int clamped = juce::jlimit (exporting::minRetentionHours,
									  exporting::maxRetentionHours, hours);

	if (clamped == getDragRetentionHours())
	{
		return;
	}

	file->setValue (kDragRetentionKey, clamped);
	lastErrorMessage = file->saveIfNeeded() ? juce::String() : writeErrorFor (settingsFile());
}

int SettingsStore::getKeyboardMiddleCOctave (void) const
{
	const int stored = file != nullptr ? file->getIntValue (kKeyboardMiddleCKey, keyboard::defaultMiddleCOctave)
									   : keyboard::defaultMiddleCOctave;

	return juce::jlimit (keyboard::minMiddleCOctave,
						 keyboard::maxMiddleCOctave, stored);
}

void SettingsStore::setKeyboardMiddleCOctave (int octave)
{
	if (file != nullptr)
	{
		file->setValue (kKeyboardMiddleCKey,
						juce::jlimit (keyboard::minMiddleCOctave,
									  keyboard::maxMiddleCOctave, octave));
	}
}

int SettingsStore::getNumericSetting (const NumericSetting& setting) const
{
	const int stored = file != nullptr ? file->getIntValue (setting.key, setting.defaultValue)
									   : setting.defaultValue;

	return juce::jlimit (setting.minimum, setting.maximum, stored);
}

void SettingsStore::setNumericSetting (const NumericSetting& setting, int value)
{
	if (file != nullptr)
	{
		file->setValue (setting.key, juce::jlimit (setting.minimum, setting.maximum, value));
	}
}

int SettingsStore::getKeyboardNoteHoldMs (void) const
{
	const auto* setting = numericSetting ("keyboard.noteHoldMs");
	return setting != nullptr ? getNumericSetting (*setting) : keyboard::defaultNoteHoldMs;
}

void SettingsStore::setKeyboardNoteHoldMs (int milliseconds)
{
	if (const auto* setting = numericSetting ("keyboard.noteHoldMs"))
	{
		setNumericSetting (*setting, milliseconds);
	}
}

int SettingsStore::getKeyboardStartNote (void) const
{
	const int stored = file != nullptr ? file->getIntValue (kKeyboardStartNoteKey, keyboard::defaultStartNote) : keyboard::defaultStartNote;

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
	return file != nullptr ? file->getBoolValue (kKeyboardRangeHintsKey, keyboard::rangeHints) : keyboard::rangeHints;
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
	return file != nullptr ? file->getIntValue (kContentViewKey, view::contentIndex) : view::contentIndex;
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
	return file != nullptr ? juce::jlimit (nsf::minOutputGainDb, nsf::maxOutputGainDb, file->getIntValue (kNsfOutputGainKey, nsf::defaultOutputGainDb))
						   : nsf::defaultOutputGainDb;
}

void SettingsStore::setNsfOutputGainDb (int decibels)
{
	const int clamped = juce::jlimit (nsf::minOutputGainDb, nsf::maxOutputGainDb, decibels);

	if (file != nullptr && clamped != getNsfOutputGainDb ())
	{
		file->setValue (kNsfOutputGainKey, clamped);
	}

	broadcastChange ();
}

int SettingsStore::getMidiPitchBendRange (void) const
{
	// Absent - a fresh install, or a file from before this setting existed - means 12: the wide
	// range most software synths ship with. The bounds come from the descriptor, which takes them
	// from the sequencer that has to obey them.
	const auto* setting = numericSetting ("midi.pitchBendRange");
	return setting != nullptr ? getNumericSetting (*setting) : midi::pitchBendRange;
}

void SettingsStore::setMidiPitchBendRange (int semitones)
{
	const auto* setting = numericSetting ("midi.pitchBendRange");

	if (setting == nullptr)
	{
		return;
	}

	const int clamped = juce::jlimit (setting->minimum, setting->maximum, semitones);

	if (clamped == getMidiPitchBendRange())
	{
		return;
	}

	setNumericSetting (*setting, clamped);
	lastErrorMessage = file->saveIfNeeded() ? juce::String() : writeErrorFor (settingsFile());

	broadcastChange();
}

bool SettingsStore::getMidiPitchBendEnabled (void) const
{
	// On by default: a chip with fine pitch (detune, vibrato, portamento) only reaches the
	// instrument through the bend wheel, and leaving it off silently quantises every song.
	return file->getBoolValue (kMidiBendEnabledKey, midi::pitchBendEnabled);
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
	return file->getBoolValue (kMidiSampleRootKey, midi::transposeBySampleRoot);
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
	file->setValue (kMidiBendRangeKey, midi::pitchBendRange);
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
