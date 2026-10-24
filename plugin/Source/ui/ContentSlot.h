#pragma once

#include "playback/ChipState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace toybox { class PlaybackEngine; }

namespace toybox::ui
{

class MixerPanel;

/** What the rectangle above the transport is showing. */
enum class ContentView
{
	mixer = 0,
	keyboard = 1,
	oscilloscope = 2
};

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

	std::unique_ptr<MixerPanel> mixer;
	std::unique_ptr<KeyboardPanel> keyboardPanel;
	std::unique_ptr<ScopePanel> scopePanel;

	std::unique_ptr<juce::FileChooser> waveChooser;
	juce::String exportStatus;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ContentSlot)
};

} // namespace toybox::ui
