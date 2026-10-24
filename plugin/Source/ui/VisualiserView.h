#pragma once

#include "View.h"

#include "i18n/LanguageManager.h"

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

	What is here now is the first of them: the scope. It reads the engine's ring of final output
	samples, so what it draws is the mix after the mixer, the master gain and the output volume -
	the same signal the host receives.
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

private:
	void timerCallback (void) override;

	juce::Rectangle<int> scopeBounds (void) const;
	juce::Rectangle<int> plansBounds (void) const;

	void paintScope (juce::Graphics& g, juce::Rectangle<int> area);

	toybox::PlaybackEngine& engine;

	// Reused between frames: the scope is redrawn thirty times a second and a fresh allocation
	// each time is the one cost this page does not need to pay.
	std::vector<float> scopeSamples;
	float scopePeak = 0.0f;
	float scopeRms = 0.0f;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VisualiserView)
};

} // namespace toybox::ui
