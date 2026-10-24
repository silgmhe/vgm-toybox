#pragma once

#include "View.h"

#include "i18n/LanguageManager.h"
#include "playback/ChipState.h"

#include <vector>

namespace toybox { class PlaybackEngine; }

namespace toybox::ui
{

/**
	The visualisation page.

	It exists so the panels that will live here can arrive one at a time without touching the
	playback page. What is here is the chip panel: a snapshot the backend fills in, copied under
	the engine's own lock rather than read out of the audio thread's registers.

	The scope that used to sit above it has been removed. It drew the final mix, which the
	transport and the mixer already show in their own way, and it made this page the busiest thing
	on screen for information the player did not need twice.
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
	void paintCardShadows (juce::Graphics& g) const override;
	void resized (void) override;
	void refreshLanguage (void) override;

private:
	struct Areas
	{
		juce::Rectangle<int> chips;
		juce::Rectangle<int> plans;
	};

	Areas layoutAreas (void) const;

	void timerCallback (void) override;

	void paintChips (juce::Graphics& g, juce::Rectangle<int> area);
	void exportWaves (void);

	toybox::PlaybackEngine& engine;
	playback::ChipStateSnapshot chipState;
	int lastChannelCount = -1;

	juce::TextButton exportButton { "export" };
	std::unique_ptr<juce::FileChooser> waveChooser;
	juce::String exportStatus;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VisualiserView)
};

} // namespace toybox::ui
