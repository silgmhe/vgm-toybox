#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace toybox::ui
{

/** Small button that draws one of the Icons glyphs instead of a text label. */
class IconButton : public juce::Button
{
public:
	enum class Icon { play, pause, stop, loop, playStart, sliders, spreadOutputs, mergeToMaster };

	IconButton (const juce::String& buttonName, Icon iconToDraw)
		: juce::Button (buttonName), icon (iconToDraw)
	{
	}

	void paintButton (juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override;

private:
	Icon icon;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IconButton)
};

} // namespace toybox::ui
