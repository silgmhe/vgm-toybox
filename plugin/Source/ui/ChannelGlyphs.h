#pragma once

#include <juce_graphics/juce_graphics.h>

#include "chips/ChipTypes.h"

namespace toybox::ui
{

/** **The seven marks are `chips::ChannelFamily`**, not a second enum that happens to have the same
	seven names.

	This header used to define `ChannelGlyph` and a 41-case `switch` to produce it, next to a
	`waveKindForChannelKind()` that did the same for waveform shapes. Adding a chip meant editing both,
	and neither would complain if one was missed. There is one table now - `chips::familyOf()` - and a
	new kind draws correctly in both places without this file being opened.

	The alias stays because the drawing functions below are generated from `assets/icon/*.svg` and
	refer to `ChannelGlyph::pulse` and friends; renaming them would churn the generated file for
	nothing.
*/
using ChannelGlyph = chips::ChannelFamily;

/** Draws `glyph` inside `area`, stroked in `colour`.

	Implemented in `ChannelGlyphs.generated.cpp`, which `plugin/tools/svg_to_channel_glyphs.py`
	produces from `plugin/assets/icon/*.svg`. The SVG is the design; that file is the drawing.
*/
void drawChannelGlyph (juce::Graphics& g, ChannelGlyph glyph,
					   juce::Rectangle<float> area, juce::Colour colour);

} // namespace toybox::ui
