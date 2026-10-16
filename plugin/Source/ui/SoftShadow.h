#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Theme.h"

namespace toybox::ui
{
/** Draws a soft, gaussian-ish shadow behind a rounded rectangle.

	Call this before filling the shape itself. The blur is skipped when the current repaint
	is entirely inside the shape - no shadow pixel can be visible there, so ordinary
	control repaints (a moving slider thumb, a label that changed its text) stay cheap and
	the blur only runs when a card or a ring around it is actually repainted.

	Note that JUCE clips every child component to its own bounds, so anything that casts a
	shadow *outside* itself has to be painted by its parent: the editor paints the card
	shadows, the transport card paints the glow of its loop button. A shadow that fits
	inside the component (a slider thumb, a tick box) can be drawn locally.

	The 6 px guard below is comfortably larger than the deepest a blur can reach back
	inside the shape once the shape's fill has covered it (the rounded corners are the
	only places that reach inside, and only by a few pixels).
*/
inline void drawSoftShadow (juce::Graphics& g, juce::Rectangle<float> area, float cornerRadius,
							juce::Colour colour, int blurRadius, juce::Point<int> offset)
{
	const auto shape = area.toNearestInt();
	const auto clip = g.getClipBounds();
	const int reach = blurRadius + juce::jmax (std::abs (offset.x), std::abs (offset.y)) + 2;

	// skip when the shadow cannot touch this repaint at all, or when the repaint sits
	// entirely inside the shape (where the fill covers every shadow pixel)
	if (! shape.expanded (reach).intersects (clip) || shape.reduced (6).contains (clip))
	{
		return;
	}

	juce::Path path;
	path.addRoundedRectangle (area, cornerRadius);
	juce::DropShadow (colour, juce::jmax (1, blurRadius), offset).drawForPath (g, path);
}

/** The two-layer shadow every card casts onto the window. */
inline void drawCardShadow (juce::Graphics& g, juce::Rectangle<float> area)
{
	drawSoftShadow (g, area, theme::cardRadius, theme::cardAmbientShadow(), theme::cardAmbientBlur, { 0, 1 });
	drawSoftShadow (g, area, theme::cardRadius, theme::cardKeyShadow(), theme::cardKeyBlur, { 0, theme::cardKeyOffsetY });
}

} // namespace toybox::ui
