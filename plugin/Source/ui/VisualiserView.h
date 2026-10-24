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
	playback page: a waveform of what you are hearing, a spectrum analyser, the song's MIDI
	activity (piano roll and a keyboard), and the FM operators and algorithms inside a chip. Each
	of those reads the engine; none of them needs to know about the transport card, the mixer or
	anything else on the page next to it - which is the whole reason the window shows one View at a
	time (ui/ViewStack.h).

	Two of them are here. The scope reads the engine's ring of final output samples, so what it
	draws is the mix after the mixer, the master gain and the output volume - the same signal the
	host receives. The chip panel reads a snapshot the backend fills in: a copy, taken under the
	engine's lock, because the alternative is reading the audio thread's registers from this one.
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
		juce::Rectangle<int> scope;
		juce::Rectangle<int> chips;
		juce::Rectangle<int> plans;
	};

	Areas layoutAreas (void) const;

	void timerCallback (void) override;

	void paintScope (juce::Graphics& g, juce::Rectangle<int> area);
	void paintChips (juce::Graphics& g, juce::Rectangle<int> area);
	void exportWaves (void);

	toybox::PlaybackEngine& engine;
	playback::ChipStateSnapshot chipState;

	juce::TextButton exportButton { "export" };
	std::unique_ptr<juce::FileChooser> waveChooser;
	juce::String exportStatus;

	// Reused between frames: the scope is redrawn thirty times a second and a fresh allocation
	// each time is the one cost this page does not need to pay.
	std::vector<float> scopeSamples;
	float scopePeak = 0.0f;
	float scopeRms = 0.0f;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VisualiserView)
};

} // namespace toybox::ui
