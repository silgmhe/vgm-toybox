#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace toybox::ui
{

/**
	The plugin's look, drawn from the palette in Theme.h: cards, buttons, sliders and the mixer's
	fader caps all ask the theme, so one instance draws either the light or the dark look - the
	class is named after neither (it was LightLookAndFeel while there was only one). One instance
	per editor (never a global default), so several plugin instances in one host do not fight over
	the process-wide LookAndFeel.

	Constructor and refresh() are the only places that read the palette.
*/
class ToyboxLookAndFeel : public juce::LookAndFeel_V4
{
public:
	/** Component property that switches a ComboBox's closed label to a compact form: the text is
		drawn without a leading "Out " (the popup and the tooltip keep the full item name). */
	static constexpr const char* compactComboTextProperty = "toybox.compactComboText";

	ToyboxLookAndFeel (void);

	/** Re-reads every colour from the palette. A LookAndFeel holds copies, so after
		theme::setTheme() this has to be called again (and the editor repainted) before the new
		palette shows. */
	void refresh (void);

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
	void drawLabel (juce::Graphics&, juce::Label&) override;

private:
	/** The vertical fader of the mixer strips (the horizontal one keeps the round thumb). */
	void drawVerticalSlider (juce::Graphics&, int x, int y, int width, int height,
							 float sliderPos, juce::Slider&);

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToyboxLookAndFeel)
};

} // namespace toybox::ui
