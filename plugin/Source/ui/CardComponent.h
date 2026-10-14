#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace vgmplay::ui
{

/**
	Base class of the white "cards" the editor is built from.

	The card paints its own background (soft shadow, white fill, hairline border), so a
	subclass only lays out its child components and - if needed - draws content on top of the
	card in paintCardContent(). Keeping the bounds and the background in the same component is
	what removes the old parallel state: the editor no longer stores card rectangles that both
	paint() and resized() have to agree on, and there is no repaint() to call by hand.
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

private:
	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CardComponent)
};

} // namespace vgmplay::ui
