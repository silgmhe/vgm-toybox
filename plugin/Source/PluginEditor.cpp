#include "PluginEditor.h"

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
}

ToyboxAudioProcessorEditor::ToyboxAudioProcessorEditor (ToyboxAudioProcessor& p)
	: AudioProcessorEditor (&p),
	  processor (p),
	  engine (p.getEngine()),
	  songInfoPanel (engine),
	  transportPanel (engine),
	  mixerPanel (engine)
{
	setLookAndFeel (&lookAndFeel);

	// --- the application menu ---
	// Close and Exit belong to the standalone build only: inside a host this window is not the
	// application, and quitting would take the DAW down with it (see ui/MainMenu.h).
	menu.standalone = (processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone);
	menu.onOpenSong = [this] { openFileChooser(); };
	menu.onCloseSong = [this] { closeSong(); };
	menu.onQuit = [] { juce::JUCEApplicationBase::quit(); };
	menu.onAbout = [this] { showAbout(); };

	// The bar and its popups both take their colours from the LookAndFeel's PopupMenu ids - that is
	// what drawMenuBarBackground()/drawMenuBarItem() look up - so this is the one place to style
	// the whole menu.
	lookAndFeel.setColour (juce::PopupMenu::backgroundColourId, theme::card());
	lookAndFeel.setColour (juce::PopupMenu::textColourId, theme::primaryText());
	lookAndFeel.setColour (juce::PopupMenu::headerTextColourId, theme::secondaryText());
	lookAndFeel.setColour (juce::PopupMenu::highlightedBackgroundColourId, theme::accent());
	lookAndFeel.setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
	addAndMakeVisible (menuBar);

	// --- open button (primary action) + status line ---
	openButton.onClick = [this] { openFileChooser(); };
	openButton.setColour (juce::TextButton::buttonColourId, theme::accent());
	openButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
	openButton.setTooltip ("Open a " + playback::BackendRegistry::getInstance().getExtensionListText() + " file");
	addAndMakeVisible (openButton);

	statusLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
	statusLabel.setColour (juce::Label::textColourId, theme::secondaryText());
	statusLabel.setJustificationType (juce::Justification::centredRight);
	addAndMakeVisible (statusLabel);

	// --- cards ---
	addAndMakeVisible (songInfoPanel);
	addAndMakeVisible (transportPanel);
	// hidden until a file with sound chips is loaded (ChipPanel::rebuild sets the visibility)
	addChildComponent (chipPanel);
	// hidden while no song with tracks is loaded (MixerPanel::refresh sets the visibility)
	addChildComponent (mixerPanel);
	// full-size overlay, hidden until a file is dragged over the editor
	addChildComponent (dropHint);

	// Help > About, drawn inside the window on purpose - ui/AboutOverlay.h has the story of what
	// a dialog of its own did to this application
	addChildComponent (aboutOverlay);
	aboutOverlay.onClose = [this] { aboutOverlay.setVisible (false); };

	rebuildChipButtons();
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
	// and start playing, which is the whole point of handing a file to this build (and, with the
	// registration script that ships in dist/, of associating the formats with it).
	// Launched with a song - Explorer's "Open with", a command line, a desktop shortcut? Load it
	// and start playing: that is the point of handing a file to this build (and, with the
	// registration script that ships in dist/, of associating the formats with it).
	startupSong = processor.getStartupFile();
	openStartupSong();

	refreshHz = kIdleRefreshHz;
	startTimerHz (refreshHz);
}

ToyboxAudioProcessorEditor::~ToyboxAudioProcessorEditor()
{
	stopTimer();
	setLookAndFeel (nullptr);
}

//==============================================================================
// painting / layout

void ToyboxAudioProcessorEditor::paint (juce::Graphics& g)
{
	// a faint vertical gradient gives the window some depth
	g.setGradientFill (juce::ColourGradient (theme::windowTop(), 0.0f, 0.0f,
											 theme::windowBottom(), 0.0f, (float) getHeight(), false));
	g.fillRect (getLocalBounds());

	// The cards and the open button are clipped to their own bounds, so the soft shadows
	// they cast onto the window have to be painted here, before them. drawSoftShadow()
	// skips the blur whenever the repaint is entirely inside a shape (a moving slider
	// thumb, a label that changed its text), so ordinary refresh repaints stay cheap.
	drawCardShadow (g, songInfoPanel.getBounds().toFloat());
	drawCardShadow (g, transportPanel.getBounds().toFloat());
	if (chipPanel.isVisible() && ! chipPanel.getBounds().isEmpty())
	{
		drawCardShadow (g, chipPanel.getBounds().toFloat());
	}
	if (mixerPanel.isVisible() && ! mixerPanel.getBounds().isEmpty())
	{
		drawCardShadow (g, mixerPanel.getBounds().toFloat());
	}
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

	// transport card (always at the bottom)
	const int transportHeight = juce::jmin (TransportPanel::preferredHeight, juce::jmax (90, area.getHeight() - 110));
	transportPanel.setBounds (area.removeFromBottom (transportHeight));
	area.removeFromBottom (theme::cardGap);

	// song info card
	const int infoHeight = juce::jmin (SongInfoPanel::preferredHeight,
									   juce::jmax (SongInfoPanel::minHeight, area.getHeight() - 80));
	songInfoPanel.setBounds (area.removeFromTop (infoHeight));
	area.removeFromTop (theme::cardGap);

	// chip mute card (+ section header) - it takes no space while it is hidden and never eats
	// into the mixer's minimum height
	if (chipPanel.hasChips())
	{
		const int chipsHeight = juce::jmin (chipPanel.getPreferredHeight (area.getWidth()),
											juce::jmax (0, area.getHeight() - MixerPanel::minimumHeight));
		if (chipsHeight > 0)
		{
			chipPanel.setBounds (area.removeFromTop (chipsHeight));
			area.removeFromTop (theme::cardGap);
		}
		else
		{
			chipPanel.setBounds ({});
		}
	}
	else
	{
		chipPanel.setBounds ({});
	}

	// The mixer card owns the whole area between the chips card and the transport card - it
	// simply follows the window height (its strips scroll inside).
	if (mixerPanel.isVisible())
	{
		mixerPanel.setBounds (area);
	}
	else
	{
		mixerPanel.setBounds ({});
	}

	dropHint.setBounds (getLocalBounds());

	if (aboutOverlay.isVisible())
	{
		aboutOverlay.setBounds (getLocalBounds());
	}

	// The card shadows are painted by paint(), outside the card bounds - a relayout has
	// to clear the ring around a card that moved, appeared or changed size.
	repaint();
}

//==============================================================================
// keyboard / drag & drop

bool ToyboxAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
	// the accelerator File > Open Song shows; the menu itself only describes it
	const auto code = key.getKeyCode();
	if ((code == 'o' || code == 'O') && key.getModifiers().isCommandDown())
	{
		openFileChooser();
		return true;
	}

	// escape dismisses the About box, like any other dialog
	if (key == juce::KeyPress::escapeKey && aboutOverlay.isVisible())
	{
		aboutOverlay.setVisible (false);
		return true;
	}

	if (key == juce::KeyPress::spaceKey)
	{
		transportPanel.togglePlayStop();
		return true;
	}
	if (key == juce::KeyPress::returnKey)
	{
		transportPanel.togglePlayPause();
		return true;
	}
	if (key == juce::KeyPress::homeKey)
	{
		transportPanel.seekToSeconds (0.0);
		return true;
	}
	if (key == juce::KeyPress::leftKey)
	{
		transportPanel.seekRelative (-5.0);
		return true;
	}
	if (key == juce::KeyPress::rightKey)
	{
		transportPanel.seekRelative (5.0);
		return true;
	}
	return false;
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
	fileChooser = std::make_unique<juce::FileChooser> ("Open a " + registry.getExtensionListText() + " file",
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
	juce::String errorMessage;
	if (! engine.loadFile (file, errorMessage))
	{
		lastError = errorMessage;
	}
	else
	{
		lastError.clear();
	}

	rebuildChipButtons();
	mixerPanel.refresh();		// the track list belongs to the new song (may show/hide the card)
	transportPanel.refresh();
	songInfoPanel.refresh();
	updateStatusLine();
	resized();					// the mixer card may have appeared or disappeared
}

void ToyboxAudioProcessorEditor::closeSong (void)
{
	engine.unloadFile();

	// the same refresh the other way round: the cards that only exist with a song loaded hide
	// themselves, the chips go away and the status line says what is open now (nothing)
	rebuildChipButtons();
	mixerPanel.refresh();
	transportPanel.refresh();
	songInfoPanel.refresh();
	updateStatusLine();
	resized();
}

void ToyboxAudioProcessorEditor::showAbout (void)
{
	aboutOverlay.setBounds (getLocalBounds());
	aboutOverlay.setVisible (true);
	aboutOverlay.toFront (false);
	aboutOverlay.grabKeyboardFocus();	// so escape works without clicking first
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

	loadFile (startupSong);

	// The audio device may not be ready yet: leave the file pending and let the timer come back.
	if (! engine.isLoaded())
	{
		return;
	}

	engine.play();
	transportPanel.refresh();
	startupHandled = true;
	// Nothing is written on success: opening a song is the normal case, and the application leaves
	// no trace anywhere unless something went wrong (see the breadcrumb in timerCallback()).
}

void ToyboxAudioProcessorEditor::rebuildChipButtons (void)
{
	chipPanel.rebuild (engine.getChips(), [this] (size_t chipIndex, bool shouldBeMuted)
										   {
											   engine.setChipMuted (chipIndex, shouldBeMuted);
										   });
	resized();	// the card may appear/disappear or change its height
}

//==============================================================================
// periodic refresh

void ToyboxAudioProcessorEditor::timerCallback (void)
{
	// Still opening the song the application was started with? The audio device may only have come
	// up in the meantime, so keep trying for about a second - and then say so once, because a song
	// that never appears is otherwise impossible to explain from the outside.
	if (! startupHandled)
	{
		if (startupAttempts++ < 12)
		{
			openStartupSong();
		}
		else
		{
			startupHandled = true;

			juce::File::getSpecialLocation (juce::File::tempDirectory)
				.getChildFile ("vgmtoybox-startup.log")
				.replaceWithText ("Could not open the song the application was started with:"
								  + juce::newLine + startupSong.getFullPathName()
								  + juce::newLine + lastError + juce::newLine);
		}
	}

	// only a running transport animates the meters and the seek bar - that is when the higher
	// rate is worth anything
	const int wantedHz = engine.isPlaying() ? kPlayingRefreshHz : kIdleRefreshHz;
	if (wantedHz != refreshHz)
	{
		refreshHz = wantedHz;
		startTimerHz (refreshHz);
	}

	transportPanel.refresh();
	songInfoPanel.refresh();
	mixerPanel.refresh();
	updateStatusLine();
}

void ToyboxAudioProcessorEditor::updateStatusLine (void)
{
	juce::String status;
	if (lastError.isNotEmpty())
	{
		status = lastError;
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

	statusLabel.setColour (juce::Label::textColourId, lastError.isNotEmpty() ? theme::error() : theme::secondaryText());
	statusLabel.setText (status, juce::dontSendNotification);
}
