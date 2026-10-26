#include "PluginEditor.h"

#include "i18n/LanguageManager.h"
#include "settings/SettingsStore.h"
#include "ui/Format.h"
#include "ui/SoftShadow.h"
#include "ui/Theme.h"

#include <atomic>

using namespace toybox;
using namespace toybox::ui;

#include "playback/BackendRegistry.h"

namespace
{
	// The UI repaints on demand, so the refresh rate is exactly what the GPU pays for: on
	// Windows every repaint makes the desktop compositor touch the window area again. 15 Hz
	// while a song plays (meters and the seek bar move) is smooth enough, and when nothing
	// moves the timer only refreshes state at a low rate - there is nothing to repaint then.
	constexpr int kPlayingRefreshHz = 15;
	constexpr int kIdleRefreshHz = 10;

	// --- paint accounting ------------------------------------------------------------------
	// Temporary probe, and deliberately not behind an environment variable: the first attempt put
	// it behind one, and a GUI binary launched from Explorer inherits whatever the shell had at
	// logon, so nothing appeared. It now always counts and always reports into the status line,
	// where it cannot be missed.
	//
	// "Is this window repainting while nothing moves?" cannot be answered by reading the code: every
	// repaint call in the refresh path is conditional, so the answer ought to be no, and a GPU
	// reading that says otherwise is either the compositor's or somebody's. One counter settles
	// which, in one run, without a profiler. See
	// docs/issue/2026-10-26-why-the-window-costs-gpu-at-rest.md.
	//
	// To remove: this block, the fetch_add in paint(), the call in timerCallback(), paintReport in
	// the header and the two lines in updateStatusLine().
	constexpr double kPaintReportSeconds = 5.0;

	std::atomic<int> editorPaints { 0 };
	std::atomic<double> lastPaintReportMs { 0.0 };

	/** How many paints since the last report, as a string to show - empty when it is not time. */
	juce::String takePaintReport (void)
	{
		const double now = juce::Time::getMillisecondCounterHiRes ();

		if (now - lastPaintReportMs.load (std::memory_order_relaxed) < kPaintReportSeconds * 1000.0)
		{
			return {};
		}

		lastPaintReportMs.store (now, std::memory_order_relaxed);

		const auto count = editorPaints.exchange (0);
		const auto line = juce::String (count) + " paints / "
						+ juce::String (kPaintReportSeconds, 0) + " s";

		juce::Logger::writeToLog ("VGMToybox paint stats: " + line);

		return line;
	}

	// What the stored mode means as a palette. "Follow system" is asked at the moment it is needed,
	// so it keeps meaning "whichever the desktop is on now" (ui/Theme.h) - and the mapping lives
	// here rather than in Theme because the stored mode is a settings type, which the theme must
	// not have to know about.
	theme::Id paletteFor (toybox::settings::SettingsStore::ThemeMode mode)
	{
		switch (mode)
		{
		case toybox::settings::SettingsStore::ThemeMode::light:
			return theme::Id::light;
		case toybox::settings::SettingsStore::ThemeMode::dark:
			return theme::Id::dark;
		case toybox::settings::SettingsStore::ThemeMode::followSystem:
			break;
		}

		return theme::systemTheme();
	}
}

ToyboxAudioProcessorEditor::ToyboxAudioProcessorEditor (ToyboxAudioProcessor& p)
	: AudioProcessorEditor (&p),
	  processor (p),
	  engine (p.getEngine())
{
	// Before anything below: a control that colours itself while it is being constructed (a label,
	// a button, a whole page) reads theme:: there and then, so the stored palette has to be in
	// force first or that control would keep the light one for its lifetime.
	i18n::LanguageManager::getInstance().initialise();
	applyPalette();

	setLookAndFeel (&lookAndFeel);

	// TooltipWindow makes itself opaque, but a tooltip only paints a rounded card. The square
	// corner pixels were therefore left black: always in the plugin and sometimes in standalone.
	// The custom tooltip LookAndFeel needs those pixels transparent for the soft shadow. Its
	// transparent margin must also ignore the mouse, or a tooltip opened below the cursor would
	// cover the control that asked for it.
	tooltipWindow.setOpaque (false);
	tooltipWindow.setInterceptsMouseClicks (false, false);

	// --- the application menu ---
	// Close and Exit belong to the standalone build only: inside a host this window is not the
	// application, and quitting would take the DAW down with it (see ui/MainMenu.h).
	menu.standalone = (processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone);

	// The window's commands own both halves of an accelerator: the key and what it runs
	// (ui/EditorCommands.h). The menu asks this table for the key it draws and renders it with
	// describe(); keyPressed() asks the same table which command claims the key - so an
	// accelerator cannot be shown but dead, or work but be shown wrong (review M-3/M-4).
	commands.add ("file.open", juce::KeyPress ('o', juce::ModifierKeys::commandModifier, 0),
				  [this] { openFileChooser(); });
	commands.add ("options.settings", juce::KeyPress (',', juce::ModifierKeys::commandModifier, 0),
				  [this] { showSettings(); });
	menu.commands = &commands;

	// The menu entries route through the same table rather than carrying a second body: one
	// command, one action, however it is reached.
	menu.onOpenSong = [this] { commands.perform ("file.open"); };
	menu.onCloseSong = [this] { closeSong(); };
	menu.onQuit = [] { juce::JUCEApplicationBase::quit(); };
	menu.onAbout = [this] { showAbout(); };
	menu.onSettings = [this] { commands.perform ("options.settings"); };

	// The menu's colours are the LookAndFeel's own (PopupMenu ids, see ToyboxLookAndFeel::refresh()):
	// its paint routines look them up there, so there is nothing to set here.
	addAndMakeVisible (menuBar);

	// --- open button (primary action) + status line ---
	// IDs for the harness: it addresses controls by name instead of guessing at bounds
	// (docs/desgin/testing-architecture.md §3.1).
	menuBar.setComponentID ("editor.menubar");
	openButton.setComponentID ("editor.open");
	statusLabel.setComponentID ("editor.status");
	openButton.onClick = [this] { openFileChooser(); };
	openButton.setButtonText (i18n::tr ("editor.open"));
	// its accent colours are applied by lookAndFeelChanged(): they are copies, so they have to be
	// re-applied whenever the palette changes
	openButton.setTooltip (formatOpenPrompt());
	addAndMakeVisible (openButton);

	statusLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
	// its colour is applied by lookAndFeelChanged(), and updateStatusLine() overrides it when the
	// line reports a failure - so it must not be set here (it would be the last word until the
	// first timer tick otherwise)
	statusLabel.setJustificationType (juce::Justification::centredRight);
	addAndMakeVisible (statusLabel);

	// --- the pages ---
	// The window owns no cards any more: ui/PlayView.h is the playback page and
	// ui/VisualiserView.h the visualisation one, and everything below the top row belongs to
	// whichever of them is on screen. The View menu is built from the stack, so a new page shows
	// up there without a line of menu code.
	auto* playPage = new PlayView (engine, processor.wrapperType != juce::AudioProcessor::wrapperType_Standalone);
	playView = playPage;
	viewStack.addView (playPage);

	// Whatever the user was looking at last time; the design's default is the mixer.
	playPage->setContentView (contentViewAt (settings::SettingsStore::getInstance ().getContentViewIndex ()));
	addAndMakeVisible (viewStack);

	// The View menu lists what the content area can show, not pages of the window: there is one
	// page, and switching replaces the rectangle above the transport.
	// Built from the registry, in its order: the ids a script invokes, the labels a user reads and
	// the index the shortcut uses all come from one table now.
	menu.viewNames.clear ();
	menu.viewIds.clear ();

	for (const auto& info : contentViews ())
	{
		menu.viewNames.add (i18n::tr (info.labelKey));
		menu.viewIds.add (info.id);
	}
	// The tick asks the page what it is showing each time the menu is built, so it follows any
	// switch - this menu, Ctrl+1..3, or a change made from code.
	menu.getCurrentView = [this]
	{
		return playView != nullptr ? contentViewIndex (playView->getContentView ()) : 0;
	};
	menu.onSelectView = [this] (int index)
	{
		if (playView != nullptr)
		{
			playView->setContentView (contentViewAt (index));
			settings::SettingsStore::getInstance ().setContentViewIndex (index);
		}
	};

	// The pages are told what the engine holds right now: an editor can be created while a song is
	// already loaded (a host reopening its window), and the chip card belongs to that song.
	viewStack.songChanged();
	songLoadedAtLastRefresh = engine.isLoaded();

	// full-size overlay, hidden until a file is dragged over the editor
	addChildComponent (dropHint);

	// Help > About, drawn inside the window on purpose - ui/AboutOverlay.h has the story of what
	// a dialog of its own did to this application
	addChildComponent (aboutOverlay);
	aboutOverlay.onClose = [this] { aboutOverlay.setVisible (false); };

	// Options > Settings, the same shape as the About box and for the same reason. The two are
	// never up together (showSettings/showAbout hide the other one), so the order they are added
	// in only decides which one an editor with both visible - which cannot happen - would be under.
	addChildComponent (settingsOverlay);
	settingsOverlay.onClose = [this] { settingsOverlay.setVisible (false); };
	// The sample-root switch changes the note numbers, so it needs the song loaded again; doing that
	// here keeps the settings page free of any knowledge of the engine.
	settingsOverlay.onSampleRootChanged = [this] { reloadForPitchChange(); };

	updateStatusLine();

	setResizable (true, true);
	// only a minimum size: the layout stretches, so the window can be resized freely and even
	// be maximised - the mixer card takes whatever the middle has to offer
	setResizeLimits (660, 520, 32768, 32768);
	setSize (820, 650);		// room for the cards down to the mixer without scrolling the window

	// The standalone window only offers minimise/close; add the maximise button so it can fill
	// the screen. Hosts manage their own windows, so this happens for the standalone wrapper
	// only (the editor copes with any size either way).
	if (processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
	{
		if (auto* window = dynamic_cast<juce::DocumentWindow*> (getTopLevelComponent()))
		{
			window->setTitleBarButtonsRequired (juce::DocumentWindow::minimiseButton
												| juce::DocumentWindow::maximiseButton
												| juce::DocumentWindow::closeButton, false);
		}
	}

	// Launched with a song - Explorer's "Open with", a command line, a desktop shortcut? Load it
	// and start playing: that is the point of handing a file to this build (and, with the
	// registration script that ships in dist/, of associating the formats with it).
	startupSong = processor.getStartupFile();
	openStartupSong();

	refreshHz = kIdleRefreshHz;
	startTimerHz (refreshHz);

	// How a theme change reaches this window, whoever made it: whichever control changes the
	// stored value, the store broadcasts, and changeListenerCallback() puts the palette into force
	// again. Nothing else can reach into this editor.
	toybox::settings::SettingsStore::getInstance().addChangeListener (this);
}

ToyboxAudioProcessorEditor::~ToyboxAudioProcessorEditor()
{
	// the store outlives the editor (it is a singleton), so the listener has to go
	toybox::settings::SettingsStore::getInstance().removeChangeListener (this);

	stopTimer();
	setLookAndFeel (nullptr);
}

//==============================================================================
// theme

void ToyboxAudioProcessorEditor::applyPalette (void)
{
	theme::setTheme (paletteFor (toybox::settings::SettingsStore::getInstance().getThemeMode()));
	lookAndFeel.refresh();
}

void ToyboxAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster* source)
{
	if (source != &toybox::settings::SettingsStore::getInstance())
	{
		return;
	}

	applyPalette();

	// The same broadcast carries a language change: re-read the stored selection (a no-op when
	// the theme was what changed) and let every control that owns text re-apply it.
	i18n::LanguageManager::getInstance().applyStoredSelection();

	openButton.setButtonText (i18n::tr ("editor.open"));
	openButton.setTooltip (formatOpenPrompt());

	menu.viewNames = viewStack.getViewNames();
	menu.viewIds = viewStack.getViewIds();
	menu.menuItemsChanged();

	viewStack.refreshLanguage();
	dropHint.refreshLanguage();
	aboutOverlay.refreshLanguage();
	settingsOverlay.refreshLanguage();

	// Both halves are needed and neither implies the other: the broadcast re-reads every colour
	// that was set through setColour() - JUCE walks the whole component tree for us, and every
	// control that colours itself does it in its own lookAndFeelChanged() - while the repaint
	// covers what is looked up while painting. The status line is re-lettered by the next timer
	// tick as well, which puts its error/secondary colour back.
	sendLookAndFeelChange();
	updateStatusLine();
	repaint();
}

void ToyboxAudioProcessorEditor::lookAndFeelChanged (void)
{
	// Everything this window colours itself, and nothing else: setColour() stores a copy, so it
	// has to be applied again whenever the palette changes. This is the hook JUCE calls for it,
	// both when the LookAndFeel is set (the constructor does that) and when it is broadcast.
	openButton.setColour (juce::TextButton::buttonColourId, theme::accent());
	openButton.setColour (juce::TextButton::textColourOffId, theme::onAccent());
	statusLabel.setColour (juce::Label::textColourId, theme::secondaryText());
}

//==============================================================================
// painting / layout

void ToyboxAudioProcessorEditor::paint (juce::Graphics& g)
{
	editorPaints.fetch_add (1, std::memory_order_relaxed);

	// a faint vertical gradient gives the window some depth
	g.setGradientFill (juce::ColourGradient (theme::windowTop(), 0.0f, 0.0f,
											 theme::windowBottom(), 0.0f, (float) getHeight(), false));
	g.fillRect (getLocalBounds());

	// The page's card shadows are painted here, not by the page: a shadow reaches outside the card
	// it belongs to, and a page's bounds end where its cards do - painted there, every shadow was
	// cut off along the cards' edges, a straight line that showed most at the bottom corners. The
	// page still says where its shadows go (View::paintCardShadows); the window has the room.
	if (auto* page = viewStack.getCurrentView())
	{
		juce::Graphics::ScopedSaveState save (g);
		g.addTransform (juce::AffineTransform::translation ((float) (viewStack.getX() + page->getX()),
															(float) (viewStack.getY() + page->getY())));

		page->paintCardShadows (g);
	}

	// What is left here is the window's own chrome: the open button and the accent glow that
	// reaches past its bounds.
	drawSoftShadow (g, openButton.getBounds().toFloat(), theme::buttonRadius,
					theme::accentGlow(), theme::accentGlowBlur, { 0, theme::accentGlowOffsetY });
}

void ToyboxAudioProcessorEditor::resized (void)
{
	auto area = getLocalBounds();
	menuBar.setBounds (area.removeFromTop (theme::menuBarHeight));
	area = area.reduced (theme::margin);

	// top row: open button + status
	auto topRow = area.removeFromTop (theme::topRowHeight);
	openButton.setBounds (topRow.removeFromLeft (140));
	topRow.removeFromLeft (theme::cardGap);
	statusLabel.setBounds (topRow);
	area.removeFromTop (theme::cardGap);

	// everything below the top row belongs to the current page, which lays itself out in there
	viewStack.setBounds (area);

	dropHint.setBounds (getLocalBounds());

	if (aboutOverlay.isVisible())
	{
		aboutOverlay.setBounds (getLocalBounds());
	}

	if (settingsOverlay.isVisible())
	{
		settingsOverlay.setBounds (getLocalBounds());
	}

	// The card shadows are painted by paint(), outside the card bounds - a relayout has
	// to clear the ring around a card that moved, appeared or changed size.
	repaint();
}

//==============================================================================
// keyboard / drag & drop

bool ToyboxAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
	// escape dismisses whichever overlay is up, like any other dialog - the settings window first,
	// since it is the one that can be over the About box and not the other way round. It comes
	// before the commands: a key pressed over an overlay belongs to the overlay.
	if (key == juce::KeyPress::escapeKey)
	{
		if (settingsOverlay.isVisible())
		{
			settingsOverlay.setVisible (false);
			return true;
		}

		if (aboutOverlay.isVisible())
		{
			aboutOverlay.setVisible (false);
			return true;
		}
	}

	// The commands own the accelerators the menus show (ui/EditorCommands.h): the menu renders
	// the key from this table, so what is shown and what is matched here cannot drift apart.
	if (commands.handleKey (key))
	{
		return true;
	}

	// Ctrl+1..3 picks a content view, in the order the View menu lists them. The key comes from
	// pageKey(), the same call the menu describes it with - and it has to do what the menu does,
	// or the accelerator works and the item it stands for does not.
	if (const int wanted = ui::EditorCommands::pageForKey (key); wanted >= 0)
	{
		if (playView != nullptr && wanted < contentViewCount ()
			&& contentViewAt (wanted) != playView->getContentView ())
		{
			playView->setContentView (contentViewAt (wanted));
			settings::SettingsStore::getInstance ().setContentViewIndex (wanted);
			return true;
		}

		return false;
	}

	// The transport keys belong to the playback page and any keys of its own to another page: a key
	// reaches the window only when the focused control did not want it, so this forwards instead
	// of duplicating what a page already knows how to do.
	return viewStack.handleKey (key);
}

bool ToyboxAudioProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
	for (const auto& path : files)
	{
		if (playback::BackendRegistry::getInstance().isSupportedFile (juce::File (path)))
		{
			return true;
		}
	}
	return false;
}

void ToyboxAudioProcessorEditor::fileDragEnter (const juce::StringArray&, int, int)
{
	dropHint.setVisible (true);
	dropHint.toFront (false);
}

void ToyboxAudioProcessorEditor::fileDragExit (const juce::StringArray&)
{
	dropHint.setVisible (false);
}

void ToyboxAudioProcessorEditor::filesDropped (const juce::StringArray& files, int, int)
{
	dropHint.setVisible (false);

	for (const auto& path : files)
	{
		if (playback::BackendRegistry::getInstance().isSupportedFile (juce::File (path)))
		{
			loadFile (juce::File (path));
			break;
		}
	}
}

//==============================================================================
// file loading

void ToyboxAudioProcessorEditor::openFileChooser (void)
{
	const juce::File startDir = engine.getFile().existsAsFile() ? engine.getFile().getParentDirectory()
																: juce::File::getSpecialLocation (juce::File::userMusicDirectory);
	// the caption and the filter come from the registry: adding a backend adds it to the dialog
	const auto& registry = playback::BackendRegistry::getInstance();
	fileChooser = std::make_unique<juce::FileChooser> (formatOpenPrompt(),
													   startDir,
													   registry.getFileWildcard());
	fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
							  [this] (const juce::FileChooser& chooser)
							  {
								  const juce::File result = chooser.getResult();
								  if (result.existsAsFile())
								  {
									  loadFile (result);
								  }
							  });
}

void ToyboxAudioProcessorEditor::loadFile (const juce::File& file, bool startPlaying)
{
	// The read and the build run on the engine's worker (review M-1): this window keeps drawing,
	// and the song that is playing keeps playing, until the new one is ready to be swapped in.
	// The status line says "Loading <name>" while that happens.
	startupHandled = true;		// this load is the startup one, or a manual one that replaces it
	loadInFlight = true;
	loadingName = file.getFileName();
	loadedFile = file;			// what `reloadForPitchChange()` loads again
	lastError.clear();			// a new attempt clears the old failure, or it would hide "Loading"
	updateStatusLine();

	// The callback comes back on the message thread, but the window may be gone by then: a
	// SafePointer stops a closed editor from being touched (the song still loads).
	juce::Component::SafePointer<ToyboxAudioProcessorEditor> safe (this);
	engine.loadFileAsync (file, [safe, file, path = loadingName]
		(bool ok, const juce::String& error)
	{
		if (safe == nullptr)
		{
			return;
		}

		safe->loadInFlight = false;
		safe->loadingName.clear();

		if (ok)
		{
			safe->lastError.clear();
			// a project that did not restore has just been superseded by this song - its message
			// is about a state that is no longer there
			safe->processor.clearStateMessage();
			safe->songLoadedAtLastRefresh = true;

			// A reload for the pitch setting puts the transport back where it was. It is done
			// here, and not before the load, because the new sequence only exists now.
			if (safe->pendingSeekSeconds >= 0.0)
			{
				safe->engine.setPositionSeconds (safe->pendingSeekSeconds);
				safe->pendingSeekSeconds = -1.0;

				if (safe->pendingResume)
				{
					safe->engine.play();
				}
				safe->pendingResume = false;
			}

			// The remembered song is written here, at the moment the load succeeds, and not at
			// shutdown: a crash or a killed process still leaves something to reopen. A plugin
			// session must never overwrite what the standalone would open next.
			if (safe->processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
			{
				toybox::settings::SettingsStore::getInstance().setLastSong (file);
			}
		}
		else
		{
			safe->lastError = path.isNotEmpty() ? (path + ": " + error) : error;
		}

		safe->viewStack.songChanged();	// every page rebuilds whatever it shows about the song
		safe->updateStatusLine();
		safe->resized();				// a page may have changed height (cards can appear)
	}, startPlaying);
}

void ToyboxAudioProcessorEditor::reloadForPitchChange (void)
{
	// The switch decides whether a sampled note carries its measured root, and that decision is
	// baked into the note numbers when the sequence is built - there is nothing to re-send. Loading
	// the song again is the same work the user used to do by hand, minus the manual step: the
	// position is remembered here and restored by the load callback.
	if (! engine.isLoaded() || ! loadedFile.existsAsFile())
	{
		return;
	}

	pendingSeekSeconds = engine.getPositionSeconds();
	pendingResume = engine.isPlaying();
	loadFile (loadedFile, false);
}

void ToyboxAudioProcessorEditor::closeSong (void)
{
	// A load that is still building must not install itself into the song that is being closed:
	// unloadFile() bumps the engine's generation, so the pending result is dropped without a
	// callback, and the "loading" state ends here.
	loadInFlight = false;
	loadingName.clear();
	engine.unloadFile();

	// the same the other way round: the cards that only exist with a song loaded hide themselves,
	// the chips go away and the status line says what is open now (nothing)
	songLoadedAtLastRefresh = false;
	viewStack.songChanged();
	updateStatusLine();
	resized();

	// Closing is the user saying "not this one": the next launch starts empty instead of opening
	// the song that was just dismissed. Only the standalone remembers anything to forget.
	if (processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
	{
		toybox::settings::SettingsStore::getInstance().clearLastSong();
	}
}

void ToyboxAudioProcessorEditor::showAbout (void)
{
	// one overlay at a time: they cover the same area, and stacking them would only hide this one
	settingsOverlay.setVisible (false);

	aboutOverlay.setBounds (getLocalBounds());
	aboutOverlay.setVisible (true);
	aboutOverlay.toFront (false);
	aboutOverlay.grabKeyboardFocus();	// so escape works without clicking first
}

void ToyboxAudioProcessorEditor::showSettings (void)
{
	// The stored values can have changed while this was hidden - that is what the Theme menu does
	// - and the pages only ever show what is stored, so the panel is put right before it comes up.
	// The store broadcasts to every window, and this is the one that has to catch up: it is the
	// only one that was not drawn while the menu was being used.
	settingsOverlay.refreshFromStore();

	aboutOverlay.setVisible (false);

	settingsOverlay.setBounds (getLocalBounds());
	settingsOverlay.setVisible (true);
	settingsOverlay.toFront (false);
	settingsOverlay.grabKeyboardFocus();	// so escape works without clicking first
}

void ToyboxAudioProcessorEditor::openStartupSong (void)
{
	if (startupHandled)
	{
		return;
	}

	if (! startupSong.existsAsFile())
	{
		startupHandled = true;		// nothing was handed to us, or it was not a song
		return;
	}

	// loadFile() starts the worker and sets startupHandled; the audio device may still be coming
	// up while that runs, and prepare() rebuilds the players at the device's rate when it does.
	// A successful load becomes the song the next launch reopens (loadFile() does that), which is
	// why this call is not different from a manual one.
	loadFile (startupSong);
}

void ToyboxAudioProcessorEditor::restoreLastSongIfNeeded (void)
{
	if (restoreAttempted)
	{
		return;
	}

	restoreAttempted = true;

	// "The last time the application was open" exists only in the standalone: a plugin's song
	// belongs to the host's project, and opening a file behind the host's back would be a bug.
	if (processor.wrapperType != juce::AudioProcessor::wrapperType_Standalone)
	{
		return;
	}

	// Something already asked for a song: the command line (the call above), a second instance
	// handing one over, a drop that beat the first tick, or a host that re-created the window with
	// a song still loaded. An explicit request wins; the restore is only the fallback.
	if (startupSong.existsAsFile() || loadInFlight || engine.isLoaded())
	{
		return;
	}

	auto& store = toybox::settings::SettingsStore::getInstance();
	if (! store.getRestoreLastSong())
	{
		return;
	}

	const auto remembered = store.getLastSong();
	if (! remembered.existsAsFile())
	{
		// Silent: the switch can be on while the file has been deleted, renamed, or left on a drive
		// that is not mounted. Nothing was asked for explicitly, so there is nothing to report.
		return;
	}

	// No autoplay: the window came back, not the playback. The fraction of a second it would take
	// to start and stop again would be the loudest thing about a feature meant to be invisible.
	loadFile (remembered, false);
}

//==============================================================================
// periodic refresh

void ToyboxAudioProcessorEditor::timerCallback (void)
{
	// The visualisation page has no transport of its own - it shows what the chips are doing to a
	// song that is already open - so the open button has nothing to act on there. Synchronised
	// here rather than in the menu callback because the page can also change by shortcut.
	{
		const auto ids = viewStack.getViewIds ();
		const int index = viewStack.getCurrentViewIndex ();
		const bool onVisualiserPage = index >= 0 && index < ids.size () && ids[index] == "visualiser";

		if (openButton.isVisible () == onVisualiserPage)
		{
			openButton.setVisible (! onVisualiserPage);
		}
	}

	// A song handed over by a second launch of the application (or, on macOS, by the Finder opening
	// a document in the running window). It goes through loadFile() like a drop does, so the two
	// paths cannot drift apart; the processor held it because the message can arrive before this
	// window exists (a document event can come in during launch).
	if (const auto handedOver = processor.takeSongFromAnotherInstance(); handedOver.existsAsFile())
	{
		loadFile (handedOver);
	}

	// Still opening the song the application was started with? openStartupSong() only starts the
	// worker (review M-1); the result arrives as a callback, so there is nothing to retry here.
	if (! startupHandled)
	{
		openStartupSong();
	}

	// After the hand-over above, never before it: on macOS the document a double-click opened is
	// delivered after this window exists, and restoring first would play the last song for a frame
	// and then replace it.
	restoreLastSongIfNeeded();

	// only a running transport animates the meters and the seek bar - that is when the higher
	// rate is worth anything
	const int wantedHz = engine.isPlaying() ? kPlayingRefreshHz : kIdleRefreshHz;
	if (wantedHz != refreshHz)
	{
		refreshHz = wantedHz;
		startTimerHz (refreshHz);
	}

	refreshUi();

	// Temporary - see the note at the top of this file.
	if (auto report = takePaintReport(); report.isNotEmpty())
	{
		paintReport = report;
	}
}

void ToyboxAudioProcessorEditor::refreshUi (void)
{
	// A song can appear (or go away) without anything in this window being clicked: the host
	// restores a project into an instance whose editor is already open. The pages that only
	// exist with a song have to be rebuilt for that, not just refreshed.
	const bool loaded = engine.isLoaded();
	if (loaded != songLoadedAtLastRefresh)
	{
		songLoadedAtLastRefresh = loaded;
		viewStack.songChanged();
		resized();
	}

	// The engine counts blocks that had to render short because the lock was held too long (a
	// state save, a load install). A stutter caused that way is invisible from the outside, so
	// say it once per window when the count moves - a log line is what makes it diagnosable
	// (review L-4).
	const int starved = engine.getStarvedBlocks();
	if (starved > reportedStarvedBlocks)
	{
		reportedStarvedBlocks = starved;
		juce::Logger::writeToLog ("VGMToybox: " + juce::String (starved)
								  + " block(s) starved so far - the engine lock was held too long");
	}

	viewStack.refresh();
	updateStatusLine();
}

void ToyboxAudioProcessorEditor::updateStatusLine (void)
{
	// A load is being built on the worker (review M-1): that is the only thing the line has to
	// say until it finishes. loadFile() cleared the previous failure, so an old error cannot
	// hide it - and on success or failure the callback comes back through here.
	if (loadInFlight)
	{
		statusLabel.setColour (juce::Label::textColourId, theme::secondaryText());
		statusLabel.setText (i18n::tr ("status.loading", loadingName), juce::dontSendNotification);
		return;
	}

	// The processor's message comes first among the non-error states: it is what a project that
	// did not restore left behind, and the only clue the user has (review §6 H-1).
	const juce::String stateMessage = processor.getStateMessage();
	const bool hasProblem = lastError.isNotEmpty() || stateMessage.isNotEmpty();

	juce::String status;
	if (lastError.isNotEmpty())
	{
		status = lastError;
	}
	else if (stateMessage.isNotEmpty())
	{
		status = stateMessage;
	}
	else if (! engine.isLoaded())
	{
		status = i18n::tr ("status.noFile");
	}
	else if (engine.isFinished())
	{
		status = i18n::tr ("status.finished");
	}
	else if (engine.isPlaying())
	{
		status = i18n::tr ("status.playing");
	}
	else
	{
		status = i18n::tr ("status.paused");
	}

	if (engine.isLoaded())
	{
		status << "   " << formatTime (engine.getPositionSeconds())
			   << " / " << formatTime (engine.getDurationSeconds());
	}

	// Temporary probe: read this off the running window while it sits idle. Change it every five
	// seconds, so it repaints about as often as a clock - not enough to matter.
	if (paintReport.isNotEmpty())
	{
		status << "   [" << paintReport << "]";
	}

	statusLabel.setColour (juce::Label::textColourId, hasProblem ? theme::error() : theme::secondaryText());
	statusLabel.setText (status, juce::dontSendNotification);
}
