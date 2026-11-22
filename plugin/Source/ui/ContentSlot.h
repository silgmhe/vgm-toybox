#pragma once

#include "playback/ChipState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace toybox { class PlaybackEngine; }

namespace toybox::ui
{

class MixerPanel;

/** What an export did - see ExportReportOverlay.h. Declared rather than included so this header
	stays as light as it is; the definition is needed only where the report is built. */
struct ExportReport;

/** What the user chose before an export started - see ExportOptionsOverlay.h. */
struct ExportOptions;

/** What the rectangle above the transport is showing. */
enum class ContentView
{
	mixer = 0,
	keyboard = 1,
	oscilloscope = 2
};

/** One row of the View menu: the view, the id a script invokes, and the label's i18n key.

	The menu, the Ctrl+1..3 accelerators and the stored index all read this table, so adding a
	view is one line here rather than an entry in four places that have to agree about order.
*/
struct ContentViewInfo
{
	ContentView view;
	const char* id;
	const char* labelKey;
};

/** The views, in menu order. */
const std::vector<ContentViewInfo>& contentViews (void);

/** The view at `index`, clamped: a settings file cannot name a view that does not exist. */
ContentView contentViewAt (int index);

/** Where `view` sits in the menu. */
int contentViewIndex (ContentView view);

int contentViewCount (void);

/**
	The swappable part of the playback page.

	Three panels live here at once and only one is visible, which is deliberate: switching views
	must not reset the mixer's faders or the chip panel's captured state, and a component that is
	merely hidden keeps all of it. The cost is that a hidden MixerPanel still refreshes - it is a
	timer tick over a handful of numbers, against the surprise of a fader jumping back.

	The slot draws the card the panels sit in, so each panel is only its contents.
*/
class ContentSlot : public juce::Component,
					private juce::Timer
{
public:
	explicit ContentSlot (toybox::PlaybackEngine& engineToUse);
	~ContentSlot (void) override;

	void setView (ContentView view);

	/** Called when an export finishes, with what it did.

		The editor owns the window that reports it, not this slot: a report dimming only this card
		read as part of the card, and left the rest of the interface looking live while it was
		blocked. Same shape as the settings panel, which covers the window.
	*/
	std::function<void (const ExportReport&)> onExportReport;

	/** Called when the export button is pressed, to ask what to write. The window that asks lives
		with the editor, for the same reason the report does. */
	std::function<void ()> onExportOptions;

	/** A wavetable drag could not produce its file. The editor owns the window that says so, which
		is the same division as the export report above. */
	std::function<void ()> onSampleDragFailed;

	/** Starts the export with the options the user chose - called back by the editor once its
		options window has been confirmed. */
	void beginExport (const ExportOptions& options);
	ContentView getView (void) const { return current; }

	/** **How many channel rows the keyboard is showing**, or -1 when that is not the question.

		Exists for the harness, and for one bug in particular: the keyboard painted an empty
		snapshot for a whole survey while `captureState` was returning eight correct rows, because
		the capture lived in a `Timer` the headless build never ran. A screenshot of a blank panel
		was reported as a success. With this the harness can fail instead - see
		`docs/issue/2026-11-22-the-instrument-that-said-no-state.md`.

		-1 on the other pages: the mixer and the scope show different things, and "no rows" there
		means nothing.
	*/
	int channelRowCount (void) const;

	void refresh (void);
	void refreshLanguage (void);
	void songChanged (void);

	void paint (juce::Graphics& g) override;
	void resized (void) override;

	/** Below this the transport would be squeezed, so the page gives the slot what is left. */
	static constexpr int minHeight = 170;

private:
	class KeyboardPanel;
	class ScopePanel;

	void timerCallback (void) override;

	/** **The export runs here, not on the message thread.** Rendering a note is a chip simulation
		and writing sixteen wav files is disk IO; at the chip's own rate a single note is 89 489
		samples, so doing it inline froze the window for as long as it took.

		Its own pool rather than the engine's `loadPool`: an export and a load would otherwise queue
		behind each other, and loading is the one the user is waiting for.
	*/
	juce::ThreadPool exportPool { juce::ThreadPool::Options{}.withThreadName ("VGMToybox export")
									  .withNumberOfThreads (1) };

	/** Shared with the pending job: the destructor clears it, so a result that arrives after this
		slot is gone is dropped instead of writing into freed state. Same shape as
		`PlaybackEngine::alive`. */
	std::shared_ptr<std::atomic<bool>> exportAlive = std::make_shared<std::atomic<bool>> (true);

	/** One export at a time. `LibvgmTrackPlayer::exportDriver` is mutable and shared, so two
		concurrent exports would rewrite the same core's registers. */
	std::atomic<bool> exporting { false };

	/** **One frame of chip-state capture**, whichever clock asked for it.

		Split out of `timerCallback` because there are two clocks and only one of them is the
		window's: the headless harness has no message loop, so `tick` goes through
		`ToyboxAudioProcessorEditor::refreshUi` → `ViewStack::refresh` → `refresh()` and never
		reaches a `Timer`. The panel then painted whatever snapshot the load left behind - empty -
		and every screenshot showed "this format does not report per-channel state" for a song that
		has eight of them. Measured on a YM2151 file: `captureState` returned eight rows every
		call while the panel had none, because nobody had asked it.
	*/
	void refreshChipState (void);

	toybox::PlaybackEngine& engine;
	ContentView current = ContentView::mixer;

	playback::ChipStateSnapshot snapshot;
	int lastChannelCount = -1;

	/** The rate the refresh timer currently runs at, read from the settings and re-read on every
		tick (see timerCallback). Zero until the constructor has asked. */
	int refreshHz = 0;

	std::unique_ptr<MixerPanel> mixer;
	std::unique_ptr<KeyboardPanel> keyboardPanel;
	std::unique_ptr<ScopePanel> scopePanel;

	std::unique_ptr<juce::FileChooser> waveChooser;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ContentSlot)
};

} // namespace toybox::ui
