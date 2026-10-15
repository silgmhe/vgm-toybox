#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "ui/ChipPanel.h"
#include "ui/DropHintOverlay.h"
#include "ui/LightLookAndFeel.h"
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
class VGMPlayAudioProcessorEditor : public juce::AudioProcessorEditor,
									private juce::Timer,
									public juce::FileDragAndDropTarget
{
public:
	explicit VGMPlayAudioProcessorEditor (VGMPlayAudioProcessor&);
	~VGMPlayAudioProcessorEditor() override;

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
	void rebuildChipButtons (void);
	void updateStatusLine (void);

	VGMPlayAudioProcessor& processor;
	vgmplay::MixerEngine& engine;

	vgmplay::ui::LightLookAndFeel lookAndFeel;

	juce::TextButton openButton { "Open VGM..." };
	juce::Label statusLabel;
	vgmplay::ui::SongInfoPanel songInfoPanel;
	vgmplay::ui::TransportPanel transportPanel;
	vgmplay::ui::ChipPanel chipPanel;
	vgmplay::ui::MixerPanel mixerPanel;
	vgmplay::ui::DropHintOverlay dropHint;

	juce::TooltipWindow tooltipWindow { this, 650 };	// declared last: destroyed first

	std::unique_ptr<juce::FileChooser> fileChooser;
	juce::String lastError;
	int refreshHz = 0;	// the rate the refresh timer currently runs at (see timerCallback())

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VGMPlayAudioProcessorEditor)
};
