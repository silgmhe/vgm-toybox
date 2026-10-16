#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "ui/AboutOverlay.h"
#include "ui/ChipPanel.h"
#include "ui/DropHintOverlay.h"
#include "ui/LightLookAndFeel.h"
#include "ui/MainMenu.h"
#include "ui/MixerPanel.h"
#include "ui/SongInfoPanel.h"
#include "ui/TransportPanel.h"

/**
	The plugin window: a top row (open button + status line) and the cards from the ui/
	module (song info, chip mutes, mixer, transport).

	The editor only assembles the panels, turns keyboard shortcuts and file drops into calls
	on the panels/engine and runs the 20 Hz refresh timer. Every panel lays itself out, paints
	itself and pulls its own state from the engine (see the refresh() methods), so there is no
	shared layout state that could go stale - the failure mode the old paint()/resized() pair
	used to have.
*/
class ToyboxAudioProcessorEditor : public juce::AudioProcessorEditor,
									private juce::Timer,
									public juce::FileDragAndDropTarget
{
public:
	explicit ToyboxAudioProcessorEditor (ToyboxAudioProcessor&);
	~ToyboxAudioProcessorEditor() override;

	void paint (juce::Graphics& g) override;
	void resized (void) override;
	bool keyPressed (const juce::KeyPress& key) override;

private:
	void timerCallback (void) override;
	bool isInterestedInFileDrag (const juce::StringArray& files) override;
	void fileDragEnter (const juce::StringArray& files, int x, int y) override;
	void fileDragExit (const juce::StringArray& files) override;
	void filesDropped (const juce::StringArray& files, int x, int y) override;

	void openFileChooser (void);
	void loadFile (const juce::File& file);

	/** Unloads the song: chips, mixer and transport go back to their "nothing loaded" state. */
	void closeSong (void);

	/** The Help > About box: what this is, which version, and who built it on whose work. */
	void showAbout (void);

	/** Opens the song the application was started with (see getStartupFile()). The audio device
		may still be coming up while the editor is built, so the first timer ticks call this again
		until the engine has it. */
	void openStartupSong (void);
	void rebuildChipButtons (void);
	void updateStatusLine (void);

	ToyboxAudioProcessor& processor;
	toybox::PlaybackEngine& engine;

	toybox::ui::LightLookAndFeel lookAndFeel;

	// menu before menuBar: the bar holds a reference to the model, so it has to go first
	toybox::ui::MainMenu menu;
	juce::MenuBarComponent menuBar { &menu };

	juce::TextButton openButton { "Open file..." };
	juce::Label statusLabel;
	toybox::ui::SongInfoPanel songInfoPanel;
	toybox::ui::TransportPanel transportPanel;
	toybox::ui::ChipPanel chipPanel;
	toybox::ui::MixerPanel mixerPanel;
	toybox::ui::DropHintOverlay dropHint;
	toybox::ui::AboutOverlay aboutOverlay;	// in-window, so it can never block the application

	juce::TooltipWindow tooltipWindow { this, 650 };	// declared last: destroyed first

	std::unique_ptr<juce::FileChooser> fileChooser;
	juce::String lastError;
	int refreshHz = 0;	// the rate the refresh timer currently runs at (see timerCallback())

	juce::File startupSong;			// what the application was launched with, if anything
	int startupAttempts = 0;
	bool startupHandled = false;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToyboxAudioProcessorEditor)
};
