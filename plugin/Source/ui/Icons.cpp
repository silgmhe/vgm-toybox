#include "Icons.h"

namespace vgmplay::ui
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

} // namespace vgmplay::ui
