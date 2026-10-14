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
	g.setColour (juce::Colours::black.withAlpha (0.05f));
	g.fillRoundedRectangle (area.translated (0.0f, 1.0f), theme::cardRadius);
	g.setColour (theme::card());
	g.fillRoundedRectangle (area, theme::cardRadius);
	g.setColour (theme::border());
	g.drawRoundedRectangle (area.reduced (0.5f), theme::cardRadius, 1.0f);

	paintCardContent (g);
}

} // namespace vgmplay::ui
