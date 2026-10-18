#include "SettingsStore.h"

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
	constexpr const char* kReuseWindowKey = "standalone.reuseWindow";
	constexpr const char* kRestoreLastSongKey = "standalone.restoreLastSong";
	constexpr const char* kLastSongKey = "standalone.lastSong";

	juce::String writeErrorFor (const juce::File& file)
	{
		return "could not write " + file.getFullPathName();
	}
}

juce::File SettingsStore::settingsDirectory (void)
{
	const auto override = juce::SystemStats::getEnvironmentVariable (kDirectoryOverrideVar, {});
	if (override.isNotEmpty())
	{
		return juce::File (override);
	}

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
	file->setValue (kReuseWindowKey, true);
	file->setValue (kRestoreLastSongKey, true);

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

} // namespace toybox::settings
