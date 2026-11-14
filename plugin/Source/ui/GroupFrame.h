#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace toybox::ui
{

/**
	The dashed frame that says "these controls belong together" - the fieldset legend of this editor.

	First drawn for the DAW SYNC cluster, where the reasoning is written out: a dashed line only ever
	means a grouping, so unlike a recess it does not have to argue that it is structure and not a
	selection rectangle. The mixer's audio switches wanted the same thing, and two copies of a dash
	pattern is two places for the dashes to drift apart, so it lives here.

	Nothing about *what* is inside is here: the caller decides the rectangle, the radius and the
	colours. The nameplate straddles the frame's top edge, so a caller laying out in the card's header
	has to leave `nameplateOverhang()` pixels above the frame for it.
*/
namespace groupFrame
{

/** Fine enough to read as a guide rather than as a border. */
inline constexpr float dashLength = 3.0f;
inline constexpr float dashGap = 3.0f;

/** The nameplate: 8 px caps, tracked out, cut into the dashes behind it. */
inline constexpr float nameplateTracking = 0.5f;
inline constexpr int nameplatePadding = 6;		// left and right of the text, inside the gap
inline constexpr int nameplateHeight = 10;		// the gap cut into the dashes behind the text

/** How far the nameplate reaches above the frame's top edge. */
inline constexpr int nameplateOverhang = nameplateHeight / 2;

/** The frame itself: 1 px dashes along a rounded rectangle, and nothing else - no fill, no inner
	shadow, no bevelled edge.

	The dashes come from createDashedStroke() on the whole outline rather than from one
	drawDashedLine() per side: only the stroked version dashes *around* the corners, and a frame
	whose corners are solid reads as a rounded rectangle with dashed sides.
*/
void draw (juce::Graphics& g, juce::Rectangle<float> area, float radius, juce::Colour colour);

/** The legend: 8 px bold caps, tracked out letter by letter, sitting *on* the frame's top edge with
	the dashes erased behind it.

	`eraser` must be the colour of the surface the frame is drawn on - the eraser is that colour, not
	a plate, because the dashes are the only thing up there and a raised plate would be a second
	surface inside a frame that deliberately has none.

	The tracking is applied per glyph (JUCE has no letter-spacing option) and the whole line is
	filled as one path, so it costs one fill and no text layout.
*/
void drawNameplate (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> frame,
					juce::Colour colour, juce::Colour eraser);

} // namespace groupFrame

} // namespace toybox::ui
