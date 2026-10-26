#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>

#include "PluginProcessor.h"
#include "ui/AboutOverlay.h"
#include "ui/DropHintOverlay.h"
#include "ui/EditorCommands.h"
#include "ui/MainMenu.h"
#include "ui/PlayView.h"
#include "ui/SettingsOverlay.h"
#include "ui/ToyboxLookAndFeel.h"
#include "ui/ViewStack.h"

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

	/** Loads `file`, and on success makes it the song the next launch reopens. `startPlaying` is
		false only for the restore below. */
	void loadFile (const juce::File& file, bool startPlaying = true);

	/** Loads the current song again at the position it is at, because `pitch.useSampleRoot` changed
		the note numbers themselves. Does nothing when there is nothing loaded - the switch then only
		applies to the next song. */
	void reloadForPitchChange (void);

	/** Unloads the song: chips, mixer and transport go back to their "nothing loaded" state. */
	void closeSong (void);

	/** The Help > About box: what this is, which version, and who built it on whose work. */
	void showAbout (void);

	/** Options > Settings (or Ctrl+,): the settings window - the same in-window overlay as the
		About box, with the theme choices and the place the settings file lives. */
	void showSettings (void);

	/** Opens the song the application was started with (see getStartupFile()). It goes through
		loadFile(), so the load itself happens on the engine's worker (review M-1) - the window is
		up and drawing while the song is built, and a load that fails says so in the status line
		instead of leaving the application silent with no explanation. */
	void openStartupSong (void);

	/** Brings back the song the standalone had loaded when it last quit, once, and only when
		nothing else asked for one (see the implementation for why it runs on the first tick
		rather than in the constructor). */
	void restoreLastSongIfNeeded (void);

	void updateStatusLine (void);

	ToyboxAudioProcessor& processor;
	toybox::PlaybackEngine& engine;

	toybox::ui::ToyboxLookAndFeel lookAndFeel;

	// commands before menu: the menu holds a pointer to the table and reads its accelerators
	toybox::ui::EditorCommands commands;

	// menu before menuBar: the bar holds a reference to the model, so it has to go first
	toybox::ui::MainMenu menu;
	juce::MenuBarComponent menuBar { &menu };

	juce::TextButton openButton { "Open file..." };
	juce::Label statusLabel;

	/** What the status line prints when the paint diagnostic is on, empty when it is off. */

	juce::String paintReport;

	/** Whether to count paints and print the count. Read from the settings on every refresh tick
		rather than per paint, so a change takes effect within a tenth of a second. See
		SettingsStore::getPaintStats(). */
	bool paintStats = false;

	/** The counters behind paintReport.

		Members rather than file-scope statics: a plugin can be instantiated on several tracks at
		once, and each editor should report its own window rather than the sum of everyone's.
	*/
	std::atomic<int> editorPaints { 0 };
	std::atomic<long long> paintedPixels { 0 };
	std::atomic<double> lastPaintReportMs { 0.0 };

	/** Paints and the area they covered since the last report - empty when it is not time yet, or
		when the diagnostic is off.

		The area is the half that matters. A count says how often the window was asked to draw; a
		count multiplied by the size of its clip says how much drawing that was. Repeated small
		rectangles and repeated whole windows are indistinguishable in a count alone.
	*/
	juce::String takePaintReport (bool enabled);

	/** The window's own background, rendered once per size and palette.

		paint() used to open by filling a vertical colour gradient across getLocalBounds(), every
		time it was called - and it is called at the refresh rate, over whatever dirty area the last
		change produced. A gradient fill is per-pixel interpolation; a blit of an image that already
		holds it is not. At 4K with a two-megapixel dirty area and fifteen refreshes a second that
		is thirty megapixels of interpolation a second for a picture that never changes.

		Rebuilt when the size changes or either end of the gradient does - both ends, because the
		image holds both.
	*/
	juce::Image backdrop;
	int backdropWidth = 0;
	int backdropHeight = 0;
	juce::uint32 backdropTop = 0;
	juce::uint32 backdropBottom = 0;

	toybox::ui::ViewStack viewStack;	// holds the pages; exactly one of them is visible
	toybox::ui::PlayView* playView = nullptr;	// the page, so the View menu can ask it what it shows
	toybox::ui::DropHintOverlay dropHint;
	toybox::ui::AboutOverlay aboutOverlay;		// in-window, so it can never block the application
	toybox::ui::SettingsOverlay settingsOverlay;	// the same, for Options > Settings

	juce::TooltipWindow tooltipWindow { this, 650 };	// declared last: destroyed first

	std::unique_ptr<juce::FileChooser> fileChooser;
	juce::String lastError;
	/** What the engine said about the song on the previous refresh: a host can hand a project to
		an instance whose window is already open, and the pages that only exist with a song are
		rebuilt on the transition (see refreshUi()). */
	bool songLoadedAtLastRefresh = false;
	int refreshHz = 0;	// the rate the refresh timer currently runs at (see timerCallback())

	juce::File startupSong;			// what the application was launched with, if anything
	bool startupHandled = false;

	/** The song the engine holds, kept so `reloadForPitchChange()` can load it again. */
	juce::File loadedFile;

	/** Where a reload has to put the transport back, and whether it was running: negative means no
		reload is pending. Consumed by the load callback, which is the only place that knows the new
		sequence is in place. */
	double pendingSeekSeconds = -1.0;
	bool pendingResume = false;

	/** The restore is a startup decision: it is tried once, after the first tick has had the
		chance to see a song handed over by another instance (macOS Finder documents). */
	bool restoreAttempted = false;

	/** A load is being built on the engine's worker right now: the status line shows which file,
		and a second request simply replaces this one (the older callback is never called). */
	bool loadInFlight = false;
	juce::String loadingName;

	/** The last starvedBlocks count this window reported (review L-4): only a move is worth a
		log line, or every tick would repeat it. */
	int reportedStarvedBlocks = 0;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToyboxAudioProcessorEditor)
};
