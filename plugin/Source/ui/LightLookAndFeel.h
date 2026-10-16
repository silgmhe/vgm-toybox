#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace toybox::ui
{

/**
	macOS-flavoured light theme: white cards on a light grey window, hairline borders, blue
	accent for anything that is "on". One instance per editor (never a global default), so
	several plugin instances in one host do not fight over the process-wide LookAndFeel.
*/
class LightLookAndFeel : public juce::LookAndFeel_V4
{
public:
	LightLookAndFeel (void);

	void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
							   bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
	void drawButtonText (juce::Graphics&, juce::TextButton&,
						 bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
	void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
						   bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
	void drawTickBox (juce::Graphics&, juce::Component&, float x, float y, float w, float h,
					  bool ticked, bool isEnabled,
					  bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
	void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
						   float sliderPos, float minSliderPos, float maxSliderPos,
						   juce::Slider::SliderStyle, juce::Slider&) override;
	void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
						   float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
						   juce::Slider&) override;
	void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
					   int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
	juce::Font getComboBoxFont (juce::ComboBox&) override;
	void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

private:
	/** The vertical fader of the mixer strips (the horizontal one keeps the round thumb). */
	void drawVerticalSlider (juce::Graphics&, int x, int y, int width, int height,
							 float sliderPos, juce::Slider&);

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LightLookAndFeel)
};

} // namespace toybox::ui
