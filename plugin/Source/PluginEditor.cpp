#include "PluginEditor.h"

#include "ui/Format.h"
#include "ui/Theme.h"

using namespace vgmplay;
using namespace vgmplay::ui;

VGMPlayAudioProcessorEditor::VGMPlayAudioProcessorEditor (VGMPlayAudioProcessor& p)
	: AudioProcessorEditor (&p),
	  processor (p),
	  engine (p.getEngine()),
	  songInfoPanel (engine),
	  transportPanel (engine)
{
	setLookAndFeel (&lookAndFeel);

	// --- open button (primary action) + status line ---
	openButton.onClick = [this] { openFileChooser(); };
	openButton.setColour (juce::TextButton::buttonColourId, theme::accent());
	openButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
	openButton.setTooltip ("Open a .vgm / .vgz file");
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
	// full-size overlay, hidden until a file is dragged over the editor
	addChildComponent (dropHint);

	rebuildChipButtons();
	updateStatusLine();

	setResizable (true, true);
	setResizeLimits (640, 420, 1400, 900);
	setSize (780, 470);
	startTimerHz (20);
}

VGMPlayAudioProcessorEditor::~VGMPlayAudioProcessorEditor()
{
	stopTimer();
	setLookAndFeel (nullptr);
}

//==============================================================================
// painting / layout

void VGMPlayAudioProcessorEditor::paint (juce::Graphics& g)
{
	g.fillAll (theme::window());
}

void VGMPlayAudioProcessorEditor::resized (void)
{
	auto area = getLocalBounds().reduced (theme::margin);

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

	// chip mute card (+ section header) - it does not take any space while it is hidden
	if (chipPanel.hasChips())
	{
		const int chipsHeight = juce::jmin (chipPanel.getPreferredHeight (area.getWidth()), area.getHeight());
		if (chipsHeight > 0)
		{
			chipPanel.setBounds (area.removeFromTop (chipsHeight));
		}
	}

	dropHint.setBounds (getLocalBounds());
}

//==============================================================================
// keyboard / drag & drop

bool VGMPlayAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
	if (key == juce::KeyPress::spaceKey)
	{
		transportPanel.togglePlayPause();
		return true;
	}
	if (key == juce::KeyPress::homeKey)
	{
		engine.setPositionSeconds (0.0);
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

bool VGMPlayAudioProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
	for (const auto& path : files)
	{
		if (path.endsWithIgnoreCase (".vgm") || path.endsWithIgnoreCase (".vgz"))
		{
			return true;
		}
	}
	return false;
}

void VGMPlayAudioProcessorEditor::fileDragEnter (const juce::StringArray&, int, int)
{
	dropHint.setVisible (true);
	dropHint.toFront (false);
}

void VGMPlayAudioProcessorEditor::fileDragExit (const juce::StringArray&)
{
	dropHint.setVisible (false);
}

void VGMPlayAudioProcessorEditor::filesDropped (const juce::StringArray& files, int, int)
{
	dropHint.setVisible (false);

	for (const auto& path : files)
	{
		if (path.endsWithIgnoreCase (".vgm") || path.endsWithIgnoreCase (".vgz"))
		{
			loadFile (juce::File (path));
			break;
		}
	}
}

//==============================================================================
// file loading

void VGMPlayAudioProcessorEditor::openFileChooser (void)
{
	const juce::File startDir = engine.getFile().existsAsFile() ? engine.getFile().getParentDirectory()
																: juce::File::getSpecialLocation (juce::File::userMusicDirectory);
	fileChooser = std::make_unique<juce::FileChooser> ("Open a VGM file", startDir, "*.vgm;*.vgz");
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

void VGMPlayAudioProcessorEditor::loadFile (const juce::File& file)
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
	transportPanel.refresh();
	songInfoPanel.refresh();
	updateStatusLine();
}

void VGMPlayAudioProcessorEditor::rebuildChipButtons (void)
{
	chipPanel.rebuild (engine.getChips(), [this] (size_t chipIndex, bool shouldBeMuted)
										   {
											   engine.setChipMuted (chipIndex, shouldBeMuted);
										   });
	resized();	// the card may appear/disappear or change its height
}

//==============================================================================
// periodic refresh

void VGMPlayAudioProcessorEditor::timerCallback (void)
{
	transportPanel.refresh();
	songInfoPanel.refresh();
	updateStatusLine();
}

void VGMPlayAudioProcessorEditor::updateStatusLine (void)
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
