#include "CardComponent.h"

#include "Theme.h"

namespace toybox::ui
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

juce::Rectangle<int> CardComponent::getCardHeaderArea (int headerHeight) const
{
	return getLocalBounds().reduced (theme::cardPadding, contentInsetY).removeFromTop (headerHeight);
}

juce::Rectangle<int> CardComponent::getCardInnerArea (int headerHeight) const
{
	auto inner = getLocalBounds().reduced (theme::cardPadding, contentInsetY);

	if (headerHeight > 0)
	{
		inner.removeFromTop (headerHeight);
	}

	return inner;
}

void CardComponent::paintCardHeaderRow (juce::Graphics& g, juce::Rectangle<int> header,
										const juce::String& title, const juce::String& rightText)
{
	if (header.isEmpty())
	{
		return;
	}

	g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
	g.setColour (theme::secondaryText());
	g.drawText (title, header, juce::Justification::centredLeft);

	if (rightText.isNotEmpty())
	{
		g.drawText (rightText, header, juce::Justification::centredRight);
	}
}

} // namespace toybox::ui
