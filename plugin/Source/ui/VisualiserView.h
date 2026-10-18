#pragma once

#include "View.h"

namespace toybox::ui
{

/**
	The visualisation page - a placeholder, deliberately so.

	It exists to make the panels that will live here addable one at a time without touching the
	playback page: a waveform of what you are hearing, a spectrum analyser, the song's MIDI
	activity (piano roll and a keyboard), and the FM operators and algorithms inside a chip. Each
	of those reads the engine; none of them needs to know about the transport card, the mixer or
	anything else on the page next to it - which is the whole reason the window now shows one View
	at a time (ui/ViewStack.h).

	Until they arrive, this paints what the page is for.
*/
class VisualiserView : public View
{
public:
	// the copy macro below suppresses the implicit default constructor
	VisualiserView (void) = default;

	juce::String getViewName (void) const override { return "Visualiser"; }

	void paint (juce::Graphics& g) override;
	void paintCardShadows (juce::Graphics& g) const override;

private:
	juce::Rectangle<int> cardBounds (void) const;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VisualiserView)
};

} // namespace toybox::ui
