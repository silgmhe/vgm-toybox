#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace vgmplay::ui
{

/** The "repeat" glyph (a ring with an arrow head on the top and the bottom edge).

	Shared between IconButton (the loop toggle), the loop point marker above the position
	slider and the drop hint area. */
void drawRepeatGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour);

} // namespace vgmplay::ui
