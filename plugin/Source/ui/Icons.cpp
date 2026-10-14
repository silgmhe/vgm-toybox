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

} // namespace vgmplay::ui
