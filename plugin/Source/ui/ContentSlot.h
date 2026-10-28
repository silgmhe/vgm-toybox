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
	ContentView getView (void) const { return current; }

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
