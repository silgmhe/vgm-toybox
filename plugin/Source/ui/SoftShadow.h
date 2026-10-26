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

/** A shadow rendered once into an image, for surfaces that repaint every frame.

	`DropShadow::drawForPath` allocates an image and runs a 2D convolution each time it is called.
	That is the right price for a static card - it is paid when the window changes - but the
	keyboard view repaints all thirteen of its rows thirty times a second whether anything moved or
	not, which is twenty-six convolutions per frame, and the guard in `drawSoftShadow()` cannot help:
	it skips the blur only when the repaint lies *inside* the shape, and repainting everything means
	the clip is the whole panel.

	This keeps the blurred result and blits it. The pixels are the same ones - the image is produced
	by the same call, on a transparent surface the size of the shadow's reach, and composited where
	it would have been drawn.

	One per shadow, owned by whoever paints. Rebuilt when the shape moves, resizes, or the palette
	changes the colour.
*/
struct ShadowImage
{
	juce::Image image;
	juce::Rectangle<int> area;
	juce::uint32 colour = 0;
	int blur = 0;
	juce::Point<int> offset;

	/** False until something has been rendered, and after clear(). */
	bool valid = false;

	void clear (void)
	{
		valid = false;
		image = juce::Image ();
	}
};

inline void drawCachedShadow (juce::Graphics& g, ShadowImage& cache,
							  juce::Rectangle<float> shape, float cornerRadius,
							  juce::Colour colour, int blurRadius, juce::Point<int> offset)
{
	const int reach = blurRadius + juce::jmax (std::abs (offset.x), std::abs (offset.y)) + 2;
	const auto bounds = shape.toNearestInt ().expanded (reach);

	// Same reason as the uncached version: a repaint that cannot show any of this should not pay
	// for the blit either.
	if (! bounds.intersects (g.getClipBounds ()))
	{
		return;
	}

	const bool stale = ! cache.valid
					|| cache.area != bounds
					|| cache.colour != colour.getARGB ()
					|| cache.blur != blurRadius
					|| cache.offset != offset;

	if (stale)
	{
		cache.image = juce::Image (juce::Image::ARGB,
								   juce::jmax (1, bounds.getWidth ()),
								   juce::jmax (1, bounds.getHeight ()), true);

		{
			juce::Graphics target (cache.image);
			drawSoftShadow (target,
							shape.translated (-(float) bounds.getX (), -(float) bounds.getY ()),
							cornerRadius, colour, blurRadius, offset);
		}

		cache.area = bounds;
		cache.colour = colour.getARGB ();
		cache.blur = blurRadius;
		cache.offset = offset;
		cache.valid = true;
	}

	g.drawImageAt (cache.image, bounds.getX (), bounds.getY ());
}

} // namespace toybox::ui
