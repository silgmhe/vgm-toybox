#include "PluginEditor.h"

#include "settings/SettingsStore.h"
#include "ui/Format.h"
#include "ui/SoftShadow.h"
#include "ui/Theme.h"

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
	applyPalette();

	setLookAndFeel (&lookAndFeel);

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
	viewStack.addView (new PlayView (engine));
	viewStack.addView (new VisualiserView());
	addAndMakeVisible (viewStack);

	menu.viewNames = viewStack.getViewNames();
	// The menu asks the stack which page is on screen every time it builds the View menu, so the
	// tick follows any switch - this menu, Ctrl+1..9, or a page changed from code.
	menu.getCurrentView = [this] { return viewStack.getCurrentViewIndex(); };
	menu.onSelectView = [this] (int index) { viewStack.setCurrentView (index); };

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
	// a faint vertical gradient gives the window some depth
	g.setGradientFill (juce::ColourGradient (theme::windowTop(), 0.0f, 0.0f,
											 theme::windowBottom(), 0.0f, (float) getHeight(), false));
	g.fillRect (getLocalBounds());

	// What belongs to a page - its cards and the shadows around them - is painted by that page
	// (see ui/PlayView.h), since only it knows what it put where. What is left here is the
	// window's own chrome: the open button and the accent glow that reaches past its bounds.
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

	// Ctrl+1..9 picks a page, in the order the View menu lists them. The key comes from pageKey(),
	// the same call the menu describes it with.
	if (const int wanted = ui::EditorCommands::pageForKey (key); wanted >= 0)
	{
		if (wanted < viewStack.getNumViews() && wanted != viewStack.getCurrentViewIndex())
		{
			viewStack.setCurrentView (wanted);
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

void ToyboxAudioProcessorEditor::loadFile (const juce::File& file)
{
	// The read and the build run on the engine's worker (review M-1): this window keeps drawing,
	// and the song that is playing keeps playing, until the new one is ready to be swapped in.
	// The status line says "Loading <name>" while that happens.
	startupHandled = true;		// this load is the startup one, or a manual one that replaces it
	loadInFlight = true;
	loadingName = file.getFileName();
	lastError.clear();			// a new attempt clears the old failure, or it would hide "Loading"
	updateStatusLine();

	// The callback comes back on the message thread, but the window may be gone by then: a
	// SafePointer stops a closed editor from being touched (the song still loads).
	juce::Component::SafePointer<ToyboxAudioProcessorEditor> safe (this);
	engine.loadFileAsync (file, [safe, path = loadingName] (bool ok, const juce::String& error)
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
		}
		else
		{
			safe->lastError = path.isNotEmpty() ? (path + ": " + error) : error;
		}

		safe->viewStack.songChanged();	// every page rebuilds whatever it shows about the song
		safe->updateStatusLine();
		safe->resized();				// a page may have changed height (cards can appear)
	});
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
	// Nothing is written on success: opening a song is the normal case, and the application leaves
	// no trace anywhere unless something went wrong (the status line says so now).
	loadFile (startupSong);
}

//==============================================================================
// periodic refresh

void ToyboxAudioProcessorEditor::timerCallback (void)
{
	// Still opening the song the application was started with? openStartupSong() only starts the
	// worker (review M-1); the result arrives as a callback, so there is nothing to retry here.
	if (! startupHandled)
	{
		openStartupSong();
	}

	// only a running transport animates the meters and the seek bar - that is when the higher
	// rate is worth anything
	const int wantedHz = engine.isPlaying() ? kPlayingRefreshHz : kIdleRefreshHz;
	if (wantedHz != refreshHz)
	{
		refreshHz = wantedHz;
		startTimerHz (refreshHz);
	}

	refreshUi();
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
		statusLabel.setText ("Loading " + loadingName + "...", juce::dontSendNotification);
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
		status = "No file";
	}
	else if (engine.isFinished())
	{
		status = "Finished";
	}
	else if (engine.isPlaying())
	{
		status = "Playing";
	}
	else
	{
		status = "Paused";
	}

	if (engine.isLoaded())
	{
		status << "   " << formatTime (engine.getPositionSeconds())
			   << " / " << formatTime (engine.getDurationSeconds());
	}

	statusLabel.setColour (juce::Label::textColourId, hasProblem ? theme::error() : theme::secondaryText());
	statusLabel.setText (status, juce::dontSendNotification);
}
