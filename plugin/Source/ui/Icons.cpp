#include "Icons.h"

#include <array>
#include <cmath>

namespace toybox::ui
{

void drawRepeatGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour)
{
	g.setColour (colour);

	juce::Path ring;
	ring.addRoundedRectangle (iconArea.getX(), iconArea.getY(), iconArea.getWidth(), iconArea.getHeight(),
							  iconArea.getWidth() * 0.34f);
	g.strokePath (ring, juce::PathStrokeType (juce::jmax (1.1f, iconArea.getWidth() * 0.12f),
											  juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

	const float head = iconArea.getWidth() * 0.36f;
	juce::Path heads;
	heads.addTriangle (iconArea.getRight() - head, iconArea.getY() - head * 0.5f,
					   iconArea.getRight() - head, iconArea.getY() + head * 0.5f,
					   iconArea.getRight(), iconArea.getY());
	heads.addTriangle (iconArea.getX() + head, iconArea.getBottom() - head * 0.5f,
					   iconArea.getX() + head, iconArea.getBottom() + head * 0.5f,
					   iconArea.getX(), iconArea.getBottom());
	g.fillPath (heads);
}

void drawPlayStartGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour)
{
	g.setColour (colour);

	const float barWidth = juce::jmax (1.2f, iconArea.getWidth() * 0.18f);
	g.fillRoundedRectangle (iconArea.removeFromLeft (barWidth), barWidth * 0.5f);

	iconArea.removeFromLeft (iconArea.getWidth() * 0.16f);
	juce::Path head;
	head.addTriangle (iconArea.getX(), iconArea.getY(),
					  iconArea.getX(), iconArea.getBottom(),
					  iconArea.getRight(), iconArea.getCentreY());
	g.fillPath (head);
}

namespace
{
	/** The three heights the fan-in/fan-out glyphs connect (top, middle, bottom lane). */
	std::array<float, 3> laneCentres (juce::Rectangle<float> iconArea, float lineWidth)
	{
		return { iconArea.getY() + lineWidth * 0.5f,
				 iconArea.getCentreY(),
				 iconArea.getBottom() - lineWidth * 0.5f };
	}
}

void drawSpreadOutputsGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour)
{
	// one source bus on the left, fanning out into three output buses
	g.setColour (colour);

	const float lineWidth = juce::jmax (1.2f, iconArea.getWidth() * 0.11f);
	const float sourceX = iconArea.getX();
	const float splitX = iconArea.getX() + iconArea.getWidth() * 0.42f;
	const float headLength = lineWidth * 2.6f;
	const float endX = iconArea.getRight() - headLength * 0.9f;
	const float centreY = iconArea.getCentreY();
	const auto lanes = laneCentres (iconArea, lineWidth);

	// the source (a vertical bar hugging the left edge)
	g.fillRoundedRectangle (sourceX, iconArea.getY(), lineWidth, iconArea.getHeight(), lineWidth * 0.5f);

	juce::Path fan;

	for (float y : lanes)
	{
		fan.startNewSubPath (sourceX + lineWidth, centreY);
		fan.lineTo (splitX, y);
		fan.lineTo (endX, y);
	}

	g.strokePath (fan, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved,
											 juce::PathStrokeType::rounded));

	for (float y : lanes)
	{
		juce::Path arrow;
		arrow.addTriangle (endX, y - headLength * 0.5f,
						   endX, y + headLength * 0.5f,
						   endX + headLength * 0.9f, y);
		g.fillPath (arrow);
	}
}

void drawMergeToMasterGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour)
{
	// three tracks on the left converging into the one master bus on the right
	g.setColour (colour);

	const float lineWidth = juce::jmax (1.2f, iconArea.getWidth() * 0.11f);
	const float headLength = lineWidth * 2.6f;
	const float startX = iconArea.getX() + headLength * 0.2f;
	const float mergeX = iconArea.getX() + iconArea.getWidth() * 0.58f;
	const float busX = iconArea.getRight() - lineWidth;
	const float centreY = iconArea.getCentreY();
	const auto lanes = laneCentres (iconArea, lineWidth);

	juce::Path merge;

	for (float y : lanes)
	{
		merge.startNewSubPath (startX, y);
		merge.lineTo (mergeX, centreY);
		merge.lineTo (busX, centreY);
	}

	g.strokePath (merge, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved,
											   juce::PathStrokeType::rounded));

	// the destination (the master bus, a vertical bar hugging the right edge)
	g.fillRoundedRectangle (busX, iconArea.getY(), lineWidth, iconArea.getHeight(), lineWidth * 0.5f);
}

void drawSlidersGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour)
{
	g.setColour (colour);

	const float lineWidth = juce::jmax (1.0f, iconArea.getWidth() * 0.10f);
	const float knobWidth = juce::jmax (3.0f, iconArea.getWidth() * 0.36f);
	const float knobHeight = juce::jmax (2.5f, iconArea.getHeight() * 0.20f);
	const float knobCentres[] = { 0.30f, 0.68f, 0.46f };

	for (int i = 0; i < 3; ++i)
	{
		const float x = iconArea.getX() + iconArea.getWidth() * (0.20f + 0.30f * (float) i);

		// the fader rail
		g.fillRoundedRectangle (x - lineWidth * 0.5f, iconArea.getY(),
								lineWidth, iconArea.getHeight(), lineWidth * 0.5f);
		// the knob, wide enough to read as one at this size
		const float knobY = iconArea.getY() + knobCentres[i] * (iconArea.getHeight() - knobHeight);
		g.fillRoundedRectangle (x - knobWidth * 0.5f, knobY, knobWidth, knobHeight, knobHeight * 0.35f);
	}
}

void drawMelodyGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour)
{
	g.setColour (colour);

	// Three white keys with two black ones: the fewest that still reads as a keyboard at 15 px.
	// Four white keys would need the black keys narrower than the gaps between them, and a key that
	// is barely wider than a separator turns the whole mark into a barcode.
	const float width = iconArea.getWidth();
	const float height = iconArea.getHeight();
	const float lineWidth = juce::jmax (1.0f, width * 0.075f);
	const float left = iconArea.getX() + width * 0.03f;
	const float right = iconArea.getRight() - width * 0.03f;
	const float top = iconArea.getY() + height * 0.14f;
	const float bottom = iconArea.getY() + height * 0.86f;
	const int whiteKeys = 3;

	juce::Path body;
	body.addRoundedRectangle (left, top, right - left, bottom - top, width * 0.10f);
	g.strokePath (body, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved,
											  juce::PathStrokeType::rounded));

	// The white-key separators run the full height of the body and the black keys are painted over
	// them: their visible part then starts exactly where a black key ends, with no gap in between,
	// which is how a keyboard seen from above looks. Two smaller things the size forces:
	//   - the separator is snapped to a pixel column (`floor (x) + 0.5f`) and drawn with the frame's
	//     pen; a one-pixel line that lands across two columns comes out as two half-lit greys, which
	//     at 15 px is no line at all;
	//   - the key is wider than the line, so it hides the line completely along its own height
	//     instead of letting a lighter stripe show through.
	const float separatorWidth = lineWidth;
	const float blackWidth = width * 0.20f;
	const float blackHeight = (bottom - top) * 0.44f;
	for (int gap = 1; gap < whiteKeys; ++gap)
	{
		const float rawX = left + (right - left) * (float) gap / (float) whiteKeys;
		const float x = std::floor (rawX) + 0.5f;
		g.drawLine (x, top, x, bottom, separatorWidth);
		g.fillRect (x - blackWidth * 0.5f, top, blackWidth, blackHeight);
	}
}

void drawDrumGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour)
{
	// A snare from the side. What makes it a cylinder rather than a box is that the body's bottom
	// is the lower half of an ellipse, not a corner: the top head curves away and the bottom
	// curves back, and the two curves are what the eye reads as round.
	const float width = iconArea.getWidth ();
	const float height = iconArea.getHeight ();
	const float lineWidth = juce::jmax (1.0f, width * 0.09f);

	const float inset = width * 0.08f;
	const float left = iconArea.getX () + inset;
	const float right = iconArea.getRight () - inset;
	const float topEdge = iconArea.getY () + height * 0.16f;
	const float bottomEdge = iconArea.getY () + height * 0.86f;

	const float ellipseHeight = (bottomEdge - topEdge) * 0.30f;
	const float topCentre = topEdge + ellipseHeight * 0.5f;
	const float bottomCentre = bottomEdge - ellipseHeight * 0.5f;

	juce::Path body;
	body.startNewSubPath (left, topCentre);
	body.lineTo (left, bottomCentre);
	body.addCentredArc ((left + right) * 0.5f, bottomCentre,
						(right - left) * 0.5f, ellipseHeight * 0.5f,
						0.0f, juce::MathConstants<float>::pi, juce::MathConstants<float>::twoPi,
						true);
	body.lineTo (right, topCentre);
	body.closeSubPath ();

	g.setColour (colour);
	g.fillPath (body);
	g.fillEllipse (juce::Rectangle<float> (left, topEdge, right - left, ellipseHeight));

	// Two light strokes: the head's own edge, and the lower hoop. Without them a filled cylinder
	// at ten pixels is a grey lozenge.
	g.setColour (colour.contrasting (0.55f));
	g.drawLine (left + lineWidth, topCentre, right - lineWidth, topCentre, lineWidth);
	g.drawLine (left, topCentre + (bottomCentre - topCentre) * 0.58f,
				right, topCentre + (bottomCentre - topCentre) * 0.58f, lineWidth);
}

void drawSampleGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour)
{
	g.setColour (colour);

	// A sample, not a note: one full-height attack and the burst that follows it, decaying. A
	// symmetric wave would read as "audio"; the flat tail is what says "one sample fired".
	const float width = iconArea.getWidth();
	const float height = iconArea.getHeight();
	const float lineWidth = juce::jmax (1.1f, width * 0.085f);
	const float centreY = iconArea.getCentreY();
	const float attackX = iconArea.getX() + width * 0.16f;
	const float right = iconArea.getRight() - width * 0.03f;

	g.drawLine (attackX, iconArea.getY() + height * 0.06f, attackX,
				iconArea.getBottom() - height * 0.06f, lineWidth);

	juce::Path wave;
	const int steps = 48;
	for (int step = 0; step <= steps; ++step)
	{
		const float t = (float) step / (float) steps;
		const float x = attackX + (right - attackX) * t;
		const float amplitude = height * 0.50f * std::exp (-2.0f * t) + height * 0.02f;
		const float y = centreY - amplitude * std::sin (t * juce::MathConstants<float>::pi * 3.0f);
		if (step == 0)
		{
			wave.startNewSubPath (x, y);
		}
		else
		{
			wave.lineTo (x, y);
		}
	}

	g.strokePath (wave, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved,
											  juce::PathStrokeType::rounded));
}

void drawMidiGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour)
{
	g.setColour (colour);

	// The DIN connector the mark stands for: the ring, five contacts across the top half and the
	// alignment notch at the bottom. Two shapes, both filled - at menu size the dots are the part
	// that makes it read as MIDI rather than as a plain circle.
	const float width = iconArea.getWidth();
	const float height = iconArea.getHeight();
	const float lineWidth = juce::jmax (1.0f, width * 0.075f);
	const float radius = juce::jmin (width, height) * 0.42f;
	const float centreX = iconArea.getCentreX();
	const float centreY = iconArea.getCentreY();

	juce::Path ring;
	ring.addEllipse (centreX - radius, centreY - radius, radius * 2.0f, radius * 2.0f);
	g.strokePath (ring, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved,
											  juce::PathStrokeType::rounded));

	// 180, 135, 90, 45, 0 degrees: the five contacts, left to right across the top half.
	const float contactAngles[] = { 180.0f, 135.0f, 90.0f, 45.0f, 0.0f };
	const float contactRadius = juce::jmax (1.0f, radius * 0.19f);
	juce::Path contacts;
	for (const float degrees : contactAngles)
	{
		const float radians = juce::degreesToRadians (degrees);
		const float x = centreX + std::cos (radians) * radius * 0.58f;
		const float y = centreY - std::sin (radians) * radius * 0.58f;
		contacts.addEllipse (x - contactRadius, y - contactRadius, contactRadius * 2.0f,
							 contactRadius * 2.0f);
	}
	g.fillPath (contacts);

	g.fillRect (centreX - radius * 0.32f, centreY + radius * 0.70f, radius * 0.64f,
				radius * 0.22f);
}

juce::Image midiGlyphImage (int pixels, juce::Colour colour)
{
	juce::Image image (juce::Image::ARGB, juce::jmax (8, pixels), juce::jmax (8, pixels), true);
	juce::Graphics g (image);
	drawMidiGlyph (g, juce::Rectangle<float> ((float) image.getWidth(), (float) image.getHeight()),
				   colour);
	return image;
}

void drawSyncGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour)
{
	// Two interlocking links, drawn as two pills overlapping in the middle: a real chain is drawn
	// on the diagonal, which at 16 px turns into an unreadable blob - two pills still read as
	// "linked" at this size.
	g.setColour (colour);

	const float thickness = juce::jmax (1.1f, iconArea.getWidth() * 0.12f);
	const float pillWidth = iconArea.getWidth() * 0.60f;
	const float pillHeight = iconArea.getHeight() * 0.48f;
	const float top = iconArea.getCentreY() - pillHeight * 0.5f;

	const auto left = juce::Rectangle<float> (iconArea.getX(), top, pillWidth, pillHeight)
						  .reduced (thickness * 0.5f);
	const auto right = juce::Rectangle<float> (iconArea.getRight() - pillWidth, top, pillWidth, pillHeight)
						   .reduced (thickness * 0.5f);

	g.drawRoundedRectangle (left, left.getHeight() * 0.5f, thickness);
	g.drawRoundedRectangle (right, right.getHeight() * 0.5f, thickness);
}

void drawCueGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour)
{
	// A flag on a pole - the marker a DAW puts on the timeline. Only two shapes, so it stays
	// sharp at 16 px, and it cannot be confused with the loop glyph next to it.
	g.setColour (colour);

	const float thickness = juce::jmax (1.2f, iconArea.getWidth() * 0.12f);
	const float poleX = iconArea.getX() + thickness * 0.5f;

	g.fillRoundedRectangle (poleX - thickness * 0.5f, iconArea.getY(), thickness, iconArea.getHeight(),
							thickness * 0.5f);

	const float pennantTop = iconArea.getY() + iconArea.getHeight() * 0.06f;
	const float pennantBottom = iconArea.getY() + iconArea.getHeight() * 0.46f;

	juce::Path pennant;
	pennant.addTriangle (poleX + thickness * 0.5f, pennantTop,
						 iconArea.getRight(), (pennantTop + pennantBottom) * 0.5f,
						 poleX + thickness * 0.5f, pennantBottom);
	g.fillPath (pennant);
}

void drawDcBlockGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour)
{
	// The response of a one-pole high pass: nothing at zero, a rise, then flat.
	juce::Path path;

	path.startNewSubPath (iconArea.getX (), iconArea.getBottom ());
	path.lineTo (iconArea.getX () + iconArea.getWidth () * 0.42f, iconArea.getY ());
	path.lineTo (iconArea.getRight (), iconArea.getY ());

	g.setColour (colour);
	g.strokePath (path, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved,
											  juce::PathStrokeType::rounded));

	// The axis it is a response against.
	g.setColour (colour.withAlpha (0.45f));
	g.fillRect (juce::Rectangle<float> (iconArea.getX (), iconArea.getBottom () - 0.8f,
										iconArea.getWidth (), 0.8f));
}

} // namespace toybox::ui
