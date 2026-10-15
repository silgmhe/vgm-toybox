#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace toybox::ui
{

/** The "repeat" glyph (a ring with an arrow head on the top and the bottom edge).

	Shared between IconButton (the loop toggle), the loop point marker above the position
	slider and the drop hint area. */
void drawRepeatGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour);

/** The "play from here" glyph (a bar followed by a right-pointing triangle) of the playback
	start cursor above the position slider. */
void drawPlayStartGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour);

/** The "mixer" glyph (three vertical faders with their knobs at different heights) of the
	mixer card's on/off toggle. */
void drawSlidersGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour);

/** The "spread outputs" glyph: one bus on the left fanning out into several output buses. */
void drawSpreadOutputsGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour);

/** The "send to master" glyph: several tracks on the left converging into one bus on the right. */
void drawMergeToMasterGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour);

} // namespace toybox::ui
