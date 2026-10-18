// The same includes JUCE's own standalone client opens with (juce_audio_plugin_client_Standalone.cpp):
// the window and holder it defines need the device, gui_extra and audio_utils modules to exist
// before the header can declare them.
#include <juce_core/system/juce_TargetPlatform.h>
#include <juce_audio_plugin_client/detail/juce_IncludeModuleHeaders.h>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

#include "PluginProcessor.h"
#include "playback/BackendRegistry.h"
#include "playback/StartupSong.h"
#include "settings/SettingsStore.h"

/**
	The standalone application, with one question answered differently from JUCE's default one
	(juce_audio_plugin_client_Standalone.cpp).

	That default says `moreThanOneInstanceAllowed() { return true; }` and has an empty
	`anotherInstanceStarted()`, so every double-clicked song is a new process. Here the first of
	those follows settings/SettingsStore.h - `standalone.reuseWindow`, on by default - and the
	second hands the song to the window that is already running.

	JUCE already does the hard part: JUCEApplicationBase takes an InterProcessLock, and a second
	instance that cannot get it broadcasts its command line to the running one and returns before
	creating anything (ApplicationBase.cpp). So the second launch costs a lock attempt, and the
	window that was already open is the one that loads the song.

	Two platform notes:

	- Windows and macOS only. On Linux `MessageManager::broadcastMessage` is an empty stub
	  (juce_Messaging_linux.cpp), so a song could not be handed over even if the second process
	  were stopped; Linux therefore keeps JUCE's answer (always a new instance) and the setting is
	  not shown.

	- macOS never starts a second process on a double-click: LaunchServices opens the document in
	  the running application, and JUCE turns that Apple event into `anotherInstanceStarted()`
	  (juce_MessageManager_mac.mm). So there the handler is the whole feature, and "a new window"
	  is not something a setting can ask for.
*/
namespace toybox
{

class ToyboxStandaloneApp final : public juce::JUCEApplication
{
public:
	ToyboxStandaloneApp (void)
	{
		juce::PropertiesFile::Options options;
		options.applicationName = JucePlugin_Name;
		options.filenameSuffix = ".settings";
		options.osxLibrarySubFolder = "Application Support";
	   #if JUCE_LINUX || JUCE_BSD
		options.folderName = "~/.config";
	   #else
		options.folderName = "";
	   #endif

		appProperties.setStorageParameters (options);
	}

	const juce::String getApplicationName (void) override { return JucePlugin_Name; }
	const juce::String getApplicationVersion (void) override { return JucePlugin_VersionString; }

	/** May a second copy of the application start?

		Only where a song can be handed over - see the note at the top of this file. */
	bool moreThanOneInstanceAllowed (void) override
	{
	   #if JUCE_WINDOWS || JUCE_MAC
		return ! settings::SettingsStore::getInstance().getReuseWindow();
	   #else
		return true;
	   #endif
	}

	/** A second launch (Windows, or a command line anywhere), or - on macOS - a document the
		Finder just opened in this running window. Either way it is a command line string, which
		playback/StartupSong.h knows how to read. */
	void anotherInstanceStarted (const juce::String& commandLine) override
	{
		const auto file = chooseSong (commandLine);

		if (! file.existsAsFile())
		{
			// The same breadcrumb the startup path leaves: "I double-clicked a song and nothing
			// happened" is otherwise impossible to diagnose from the outside.
			writeBreadcrumb (commandLine, "No argument was a supported song file.");
			return;
		}

		// macOS can deliver the document while the application is still starting - there is no
		// window to hand it to yet. It is the same launch, so it waits for initialise().
		if (mainWindow == nullptr)
		{
			pendingSong = file;
			return;
		}

		handOver (file);
	}

	void initialise (const juce::String&) override
	{
		mainWindow = juce::rawToUniquePtr (createWindow());

		if (mainWindow != nullptr)
		{
			mainWindow->setVisible (true);

			if (pendingSong.existsAsFile())
			{
				handOver (pendingSong);
				// juce::File(), not {}: File has both copy and move assignment, and clang reads
				// the empty braces as a tie between them. MSVC takes one, so only clang builds failed.
				pendingSong = juce::File();
			}
		}
		else
		{
			// No display to open a window on (JUCE's own default does the same): keep the holder
			// alive so the application still starts and can report it.
			pluginHolder = createPluginHolder();
		}
	}

	void shutdown (void) override
	{
		pluginHolder = nullptr;
		mainWindow = nullptr;
		appProperties.saveIfNeeded();
	}

	void systemRequestedQuit (void) override
	{
		if (pluginHolder != nullptr)
		{
			pluginHolder->savePluginState();
		}

		if (mainWindow != nullptr)
		{
			mainWindow->pluginHolder->savePluginState();
		}

		if (juce::ModalComponentManager::getInstance()->cancelAllModalComponents())
		{
			juce::Timer::callAfterDelay (100, []
			{
				if (auto* app = juce::JUCEApplicationBase::getInstance())
				{
					app->systemRequestedQuit();
				}
			});
		}
		else
		{
			quit();
		}
	}

private:
	juce::StandaloneFilterWindow* createWindow (void)
	{
		if (juce::Desktop::getInstance().getDisplays().displays.isEmpty())
		{
			return nullptr;
		}

		return new juce::StandaloneFilterWindow (getApplicationName(),
												 juce::LookAndFeel::getDefaultLookAndFeel()
													 .findColour (juce::ResizableWindow::backgroundColourId),
												 createPluginHolder());
	}

	std::unique_ptr<juce::StandalonePluginHolder> createPluginHolder (void)
	{
		return std::make_unique<juce::StandalonePluginHolder> (appProperties.getUserSettings(),
															   false,
															   juce::String(),
															   nullptr,
															   juce::Array<juce::StandalonePluginHolder::PluginInOuts>(),
															   false);
	}

	/** The song in that command line, by the rule the startup path uses. */
	juce::File chooseSong (const juce::String& commandLine) const
	{
		const auto& registry = playback::BackendRegistry::getInstance();

		return playback::chooseSongFromInstance (commandLine, [&registry] (const juce::File& file)
																	{
																		return registry.isSupportedFile (file);
																	});
	}

	/** Gives the file to the window and brings it forward: a double-click is a request to see the
		song, and on Windows nothing else would raise the window. */
	void handOver (const juce::File& file)
	{
		// A standalone window of this application holds this processor and no other, so the cast
		// is the same one juce_StandaloneFilterWindow makes in its own editor code.
		static_cast<ToyboxAudioProcessor*> (mainWindow->getAudioProcessor())
			->openSongFromAnotherInstance (file);

		mainWindow->toFront (true);

		writeBreadcrumb (file.getFullPathName(), "Handed to the running window.");
	}

	/** One line in the temp folder for every hand-over, and for every one that could not happen:
		this is the only outside view of a feature whose whole job is that a second window does not
		appear. The startup path leaves the same kind of note (PluginProcessor::getStartupFile). */
	void writeBreadcrumb (const juce::String& commandLine, const juce::String& result)
	{
		const auto& store = settings::SettingsStore::getInstance();

		juce::File::getSpecialLocation (juce::File::tempDirectory)
			.getChildFile ("vgmtoybox-second-instance.log")
			.replaceWithText ("Another instance started with: " + commandLine + juce::newLine
							  + result + juce::newLine
							  + "reuse setting: " + (store.getReuseWindow() ? "on" : "off") + juce::newLine
							  + "settings file: " + settings::SettingsStore::settingsFile().getFullPathName() + juce::newLine
							  + "settings error: " + store.lastError() + juce::newLine);
	}

	juce::ApplicationProperties appProperties;
	std::unique_ptr<juce::StandaloneFilterWindow> mainWindow;
	std::unique_ptr<juce::StandalonePluginHolder> pluginHolder;	// only when no window could be made
	juce::File pendingSong;	// a document that arrived before the window did
};

} // namespace toybox

JUCE_CREATE_APPLICATION_DEFINE (toybox::ToyboxStandaloneApp)
