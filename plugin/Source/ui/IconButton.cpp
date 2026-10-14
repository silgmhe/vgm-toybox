#include "IconButton.h"

#include "Icons.h"
#include "Theme.h"

namespace vgmplay::ui
{

void IconButton::paintButton (juce::Graphics& g, bool isMouseOverButton, bool isButtonDown)
{
	const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
	const bool toggled = getToggleState();

	auto background = toggled ? findColour (juce::TextButton::buttonOnColourId)
							 : findColour (juce::TextButton::buttonColourId);
	auto iconColour = toggled ? findColour (juce::TextButton::textColourOnId)
							  : findColour (juce::TextButton::textColourOffId);

	if (! isEnabled())
	{
		background = background.withMultipliedAlpha (0.5f);
		iconColour = iconColour.withMultipliedAlpha (0.4f);
	}
	else if (isButtonDown)
	{
		background = background.darker (0.10f);
	}
	else if (isMouseOverButton)
	{
		background = background.darker (0.05f);
	}

	g.setColour (background);
	g.fillRoundedRectangle (bounds, theme::buttonRadius);

	if (! toggled)
	{
		g.setColour (theme::border());
		g.drawRoundedRectangle (bounds.reduced (0.5f), theme::buttonRadius, 1.0f);
	}

	const auto iconArea = bounds.reduced (bounds.getWidth() * 0.28f, bounds.getHeight() * 0.28f);
	g.setColour (iconColour);

	if (icon == Icon::play)
	{
		juce::Path path;
		path.addTriangle (iconArea.getX(), iconArea.getY(),
						  iconArea.getX(), iconArea.getBottom(),
						  iconArea.getRight(), iconArea.getCentreY());
		g.fillPath (path);
	}
	else if (icon == Icon::pause)
	{
		juce::Path path;
		const float barWidth = iconArea.getWidth() * 0.32f;
		path.addRectangle (iconArea.getX(), iconArea.getY(), barWidth, iconArea.getHeight());
		path.addRectangle (iconArea.getRight() - barWidth, iconArea.getY(), barWidth, iconArea.getHeight());
		g.fillPath (path);
	}
	else if (icon == Icon::stop)
	{
		g.fillRoundedRectangle (iconArea, 1.5f);
	}
	else if (icon == Icon::playStart)
	{
		drawPlayStartGlyph (g, iconArea, iconColour);
	}
	else
	{
		drawRepeatGlyph (g, iconArea, iconColour);
	}
}

} // namespace vgmplay::ui
