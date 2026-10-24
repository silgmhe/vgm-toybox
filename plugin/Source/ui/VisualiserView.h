#pragma once

#include "View.h"

#include "i18n/LanguageManager.h"
#include "playback/ChipState.h"

#include <memory>

namespace toybox { class PlaybackEngine; }

namespace toybox::ui
{

/**
	The visualisation page, as two tabs.

	"The chip" shows what each channel is doing: its note, drawn on a piano keyboard as a lit key,
	its level, and - for the chips that read one - the waveform it is playing. "The oscilloscope"
	is a placeholder; it will show the output signal and has nothing in it yet.

	Both read the same snapshot the backend fills in, copied under the engine's own lock rather
	than read out of the audio thread's registers. Which tab is showing does not change the cost:
	the capture runs once per frame for the page, not per tab.
*/
class VisualiserView : public View,
					   private juce::Timer
{
public:
	explicit VisualiserView (toybox::PlaybackEngine& engineToUse);
	~VisualiserView (void) override;

	juce::String getViewId (void) const override { return "visualiser"; }
	juce::String getViewName (void) const override { return toybox::i18n::tr ("view.visualiser"); }

	void paint (juce::Graphics& g) override;
	void resized (void) override;
	void refreshLanguage (void) override;

private:
	class ChipPanel;
	class ScopePanel;

	void timerCallback (void) override;
	void exportWaves (void);

	toybox::PlaybackEngine& engine;
	playback::ChipStateSnapshot chipState;
	int lastChannelCount = -1;

	juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
	std::unique_ptr<ChipPanel> chipPanel;
	std::unique_ptr<ScopePanel> scopePanel;

	std::unique_ptr<juce::FileChooser> waveChooser;
	juce::String exportStatus;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VisualiserView)
};

} // namespace toybox::ui
