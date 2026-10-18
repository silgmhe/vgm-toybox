#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "TrayMenu.h"

namespace toybox::standalone
{

/** Draws a transparent-background tray menu glyph in a single ink colour.

	The glyph is drawn with the caller's ink colour, so the same image reads correctly on light
	and dark menus. There is deliberately no background plate: the menu's own background (or
	accent highlight) shows through every pixel that is not part of the glyph.

	When @p ticked is true, a small filled check badge is included because JUCE replaces the
	native tick with the item image. The check itself is cut out of the badge, so it also stays
	correct over an accent-coloured highlight.

	@pixelSize is the raster size in physical pixels; the glyph is designed on a 16 x 16 grid. */
juce::Image makeTrayMenuIcon (TrayMenuIcon icon,
							  int pixelSize,
							  bool ticked,
							  juce::Colour core,
							  float opacity = 1.0f);

/** The monochrome variant for an NSImage marked as a template. AppKit supplies the colour (and
	the disabled/highlighted appearance), so the image only carries the glyph's alpha. */
juce::Image makeTrayMenuIconTemplate (TrayMenuIcon icon, int pixelSize);

} // namespace toybox::standalone
