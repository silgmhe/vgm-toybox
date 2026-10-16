#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace toybox::ui
{

/**
	Base class of the white "cards" the editor is built from.

	The card paints its own background (white fill, hairline border), so a subclass only
	lays out its child components and - if needed - draws content on top of the card in
	paintCardContent(). Keeping the bounds and the background in the same component is
	what removes the old parallel state: the editor no longer stores card rectangles that
	both paint() and resized() have to agree on, and there is no repaint() to call by hand.

	The soft shadow behind the card is painted by the editor (see its paint()), not here:
	JUCE clips every component to its own bounds, so the card could not draw outside itself.
*/
class CardComponent : public juce::Component
{
public:
	// the copy/move macros below suppress the implicit default constructor
	CardComponent (void) = default;

	void paint (juce::Graphics& g) override;

protected:
	/** Called after the card background has been drawn - draw card content here. */
	virtual void paintCardContent (juce::Graphics&) {}

	/** The vertical inset between a card's edge and its content (the horizontal one is
		theme::cardPadding). */
	static constexpr int contentInsetY = 8;

	/** Height of a header row that holds only text. A card whose header holds buttons reserves
		their size instead, so this is the common case rather than a rule. */
	static constexpr int textHeaderHeight = 20;

	/** The header row: the bounds inset by the standard padding, its top `headerHeight` pixels.
		Empty when there is no room. */
	juce::Rectangle<int> getCardHeaderArea (int headerHeight) const;

	/** The area a card's content owns: the same inset bounds, less that header row. Both cards used
		to spell this out themselves, and the visualisation page would have made it a third copy of
		the same four lines. */
	juce::Rectangle<int> getCardInnerArea (int headerHeight = 0) const;

	/** Paints a header row: a bold caption on the left, optional text on the right. */
	static void paintCardHeaderRow (juce::Graphics& g, juce::Rectangle<int> header,
									const juce::String& title, const juce::String& rightText = {});

private:
	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CardComponent)
};

} // namespace toybox::ui
