#include "GroupFrame.h"

#include <cmath>

namespace toybox::ui::groupFrame
{

void draw (juce::Graphics& g, juce::Rectangle<float> area, float radius, juce::Colour colour)
{
	if (area.isEmpty ())
	{
		return;
	}

	juce::Path outline;
	outline.addRoundedRectangle (area.reduced (0.5f), radius);	// the 1 px stroke lands on the grid

	const float dashes[] = { dashLength, dashGap };
	juce::Path stroked;
	juce::PathStrokeType (1.0f).createDashedStroke (stroked, outline, dashes, 2);

	g.setColour (colour);
	g.fillPath (stroked);
}

void drawNameplate (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> frame,
					juce::Colour colour, juce::Colour eraser)
{
	if (frame.isEmpty () || text.isEmpty ())
	{
		return;
	}

	const juce::Font font { juce::FontOptions (8.0f, juce::Font::bold) };

	juce::GlyphArrangement glyphs;
	glyphs.addLineOfText (font, text, 0.0f, 0.0f);	// baseline at y = 0
	for (int i = 0; i < glyphs.getNumGlyphs (); ++i)
	{
		glyphs.getGlyph (i).moveBy (nameplateTracking * (float) i, 0.0f);
	}

	juce::Path path;
	glyphs.createPath (path);
	const auto glyphBounds = path.getBounds ();

	// the gap in the dashes, centred on the frame
	const int gapWidth = juce::jmin (frame.getWidth (),
									 (int) std::ceil (glyphBounds.getWidth ()) + 2 * nameplatePadding);
	const juce::Rectangle<float> gap ((float) frame.getCentreX () - (float) gapWidth * 0.5f,
									  (float) frame.getY () - (float) nameplateHeight * 0.5f,
									  (float) gapWidth, (float) nameplateHeight);

	g.setColour (eraser);
	g.fillRoundedRectangle (gap, 2.0f);

	// Both axes centre the glyph box, not the baseline: the names are all caps, so the box is the
	// visible ink and centring it is what looks centred.
	const auto shift = gap.getCentre () - glyphBounds.getCentre ();

	g.setColour (colour);
	g.fillPath (path, juce::AffineTransform::translation (shift.x, shift.y));
}

} // namespace toybox::ui::groupFrame
