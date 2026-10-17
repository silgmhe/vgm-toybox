#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "ui/AboutOverlay.h"
#include "ui/DropHintOverlay.h"
#include "ui/MainMenu.h"
#include "ui/PlayView.h"
#include "ui/SettingsOverlay.h"
#include "ui/ToyboxLookAndFeel.h"
#include "ui/ViewStack.h"
#include "ui/VisualiserView.h"

/**
	The plugin window: the menu bar, a top row (open button + status line), and then one page.

	Everything below the top row belongs to a View (ui/View.h): the playback page with the song
	info, chip mutes, mixer and transport cards, or the visualisation page that is being built.
	The window keeps only what is genuinely global to it - the menus, the open button, the status
	line, the file drops and the refresh timer - and hands the rest to the stack, so adding a page
	means writing a View and adding it there (the View menu is built from the stack).

	No shared layout state: the window lays out its own three pieces, and each page lays out the
	controls it owns.
*/
class ToyboxAudioProcessorEditor : public juce::AudioProcessorEditor,
									private juce::Timer,
									private juce::ChangeListener,
									public juce::FileDragAndDropTarget
{
public:
	explicit ToyboxAudioProcessorEditor (ToyboxAudioProcessor&);
	~ToyboxAudioProcessorEditor() override;

	void paint (juce::Graphics& g) override;
	void resized (void) override;
	bool keyPressed (const juce::KeyPress& key) override;

	/** One frame of the window's own work: refresh the page and the status line.

		The timer calls it - and so can the console harness, which is how state that only a refresh
		produces (a lit transport button, a meter) became assertable without running a message loop
		(docs/desgin/testing-architecture.md §7, risk 2). */
	void refreshUi (void);

private:
	void timerCallback (void) override;
	void changeListenerCallback (juce::ChangeBroadcaster* source) override;

	/** Re-reads the palette into the controls that carry a colour of their own.
		setColour() stores a copy, so a theme change does not reach them by itself - this is the
		hook JUCE calls for exactly that, both when the LookAndFeel is set and when it is
		broadcast (sendLookAndFeelChange() walks the whole tree). */
	void lookAndFeelChanged (void) override;

	/** Puts the stored theme (resolving "follow system" against the desktop) into force and
		refreshes the LookAndFeel's copies of it. Called from the constructor, before anything is
		coloured, and again whenever the settings broadcast arrives. */
	void applyPalette (void);

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

	/** Options > Settings (or Ctrl+,): the settings window - the same in-window overlay as the
		About box, with the theme choices and the place the settings file lives. */
	void showSettings (void);

	/** Opens the song the application was started with (see getStartupFile()). The audio device
		may still be coming up while the editor is built, so the first timer ticks call this again
		until the engine has it. */
	void openStartupSong (void);
	void updateStatusLine (void);

	ToyboxAudioProcessor& processor;
	toybox::PlaybackEngine& engine;

	toybox::ui::ToyboxLookAndFeel lookAndFeel;

	// menu before menuBar: the bar holds a reference to the model, so it has to go first
	toybox::ui::MainMenu menu;
	juce::MenuBarComponent menuBar { &menu };

	juce::TextButton openButton { "Open file..." };
	juce::Label statusLabel;
	toybox::ui::ViewStack viewStack;	// holds the pages; exactly one of them is visible
	toybox::ui::DropHintOverlay dropHint;
	toybox::ui::AboutOverlay aboutOverlay;		// in-window, so it can never block the application
	toybox::ui::SettingsOverlay settingsOverlay;	// the same, for Options > Settings

	juce::TooltipWindow tooltipWindow { this, 650 };	// declared last: destroyed first

	std::unique_ptr<juce::FileChooser> fileChooser;
	juce::String lastError;
	int refreshHz = 0;	// the rate the refresh timer currently runs at (see timerCallback())

	juce::File startupSong;			// what the application was launched with, if anything
	int startupAttempts = 0;
	bool startupHandled = false;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToyboxAudioProcessorEditor)
};
