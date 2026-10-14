#include "CardComponent.h"

#include "Theme.h"

namespace vgmplay::ui
{

void CardComponent::paint (juce::Graphics& g)
{
	if (getLocalBounds().isEmpty())
	{
		return;
	}

	const auto area = getLocalBounds().toFloat();
	g.setColour (theme::card());
	g.fillRoundedRectangle (area, theme::cardRadius);
	g.setColour (theme::cardBorder());
	g.drawRoundedRectangle (area.reduced (0.5f), theme::cardRadius, 1.0f);

	paintCardContent (g);
}

} // namespace vgmplay::ui
