#include "LightLookAndFeel.h"

#include "SoftShadow.h"
#include "Theme.h"

namespace vgmplay::ui
{

LightLookAndFeel::LightLookAndFeel (void)
{
	setColourScheme (juce::LookAndFeel_V4::getLightColourScheme());

	setColour (juce::ResizableWindow::backgroundColourId,		theme::window());
	setColour (juce::Label::textColourId,						theme::primaryText());

	setColour (juce::TextButton::buttonColourId,				theme::card());
	setColour (juce::TextButton::buttonOnColourId,				theme::accent());
	setColour (juce::TextButton::textColourOffId,				theme::primaryText());
	setColour (juce::TextButton::textColourOnId,				juce::Colours::white);

	setColour (juce::ComboBox::backgroundColourId,				theme::card());
	setColour (juce::ComboBox::textColourId,					theme::primaryText());
	setColour (juce::ComboBox::outlineColourId,					juce::Colours::transparentBlack);
	setColour (juce::ComboBox::arrowColourId,					theme::secondaryText());

	setColour (juce::PopupMenu::backgroundColourId,				theme::card());
	setColour (juce::PopupMenu::textColourId,					theme::primaryText());
	setColour (juce::PopupMenu::highlightedBackgroundColourId,	theme::accent());
	setColour (juce::PopupMenu::highlightedTextColourId,		juce::Colours::white);

	setColour (juce::Slider::backgroundColourId,				theme::track());
	setColour (juce::Slider::trackColourId,						theme::accent());
	setColour (juce::Slider::thumbColourId,						juce::Colours::white);
	setColour (juce::Slider::textBoxTextColourId,				theme::primaryText());
	setColour (juce::Slider::textBoxBackgroundColourId,			juce::Colours::transparentBlack);
	setColour (juce::Slider::textBoxOutlineColourId,			juce::Colours::transparentBlack);
	setColour (juce::Slider::textBoxHighlightColourId,			theme::accent().withAlpha (0.25f));

	setColour (juce::ToggleButton::textColourId,				theme::primaryText());
	setColour (juce::ToggleButton::tickColourId,				theme::accent());
	setColour (juce::ToggleButton::tickDisabledColourId,		theme::border());

	setColour (juce::TooltipWindow::backgroundColourId,			juce::Colour (0xf7ffffff));
	setColour (juce::TooltipWindow::textColourId,				theme::primaryText());
	setColour (juce::TooltipWindow::outlineColourId,			theme::border());
}

void LightLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
	const juce::Colour& backgroundColour, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
	const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
	auto fill = backgroundColour.withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f);

	if (shouldDrawButtonAsDown)
	{
		fill = fill.darker (0.10f);
	}
	else if (shouldDrawButtonAsHighlighted)
	{
		fill = fill.darker (0.05f);
	}

	// small square buttons (the mixer's M/S) only get a slight corner rounding - the radius
	// never exceeds a fifth of the button height
	const float radius = (bounds.getHeight() < 28.0f) ? bounds.getHeight() * 0.2f : theme::buttonRadius;

	g.setColour (fill);
	g.fillRoundedRectangle (bounds, radius);

	// a hairline keeps light buttons readable; a filled accent button does not need one
	if (fill.getPerceivedBrightness() > 0.6f && ! button.getToggleState())
	{
		g.setColour (theme::border());
		g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.0f);
	}
}

void LightLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button,
	bool /*shouldDrawButtonAsHighlighted*/, bool /*shouldDrawButtonAsDown*/)
{
	const auto area = button.getLocalBounds();

	// short buttons (the mixer's M/S) get a proportionally smaller label, so even a narrow
	// button never elides its text
	const float fontSize = juce::jlimit (10.0f, 13.5f, (float) area.getHeight() * 0.62f);
	g.setFont (juce::Font (juce::FontOptions (fontSize, juce::Font::bold)));
	g.setColour (button.findColour (button.getToggleState() ? juce::TextButton::textColourOnId
															: juce::TextButton::textColourOffId)
					 .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));
	g.drawFittedText (button.getButtonText(), area.reduced (4, 0),
					  juce::Justification::centred, 1, 0.7f);
}

void LightLookAndFeel::drawTickBox (juce::Graphics& g, juce::Component& component,
	float x, float y, float w, float h, bool ticked, bool isEnabled,
	bool shouldDrawButtonAsHighlighted, bool /*shouldDrawButtonAsDown*/)
{
	const auto box = juce::Rectangle<float> (x, y, w, h).reduced (w * 0.1f);
	constexpr float radius = 4.0f;

	if (ticked)
	{
		const auto tickedColour = isEnabled ? component.findColour (juce::ToggleButton::tickColourId)
											: component.findColour (juce::ToggleButton::tickDisabledColourId);
		drawSoftShadow (g, box, radius, theme::controlShadow().withMultipliedAlpha (isEnabled ? 1.0f : 0.45f),
						theme::controlShadowBlur, { 0, 1 });
		g.setColour (tickedColour);
		g.fillRoundedRectangle (box, radius);

		juce::Path tick;
		const float size = box.getWidth();
		tick.startNewSubPath (box.getX() + size * 0.24f, box.getY() + size * 0.52f);
		tick.lineTo (box.getX() + size * 0.43f, box.getY() + size * 0.71f);
		tick.lineTo (box.getX() + size * 0.78f, box.getY() + size * 0.29f);
		g.setColour (juce::Colours::white);
		g.strokePath (tick, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
	}
	else
	{
		g.setColour (juce::Colours::white);
		g.fillRoundedRectangle (box, radius);
		g.setColour (shouldDrawButtonAsHighlighted ? theme::secondaryText().withAlpha (0.6f) : juce::Colour (0xffc5c5cb));
		g.drawRoundedRectangle (box.reduced (0.5f), radius, 1.0f);
	}
}

void LightLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
	bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
	const auto area = button.getLocalBounds().toFloat();
	const float boxSize = juce::jmin (15.0f, area.getHeight() - 4.0f);
	const auto box = juce::Rectangle<float> (boxSize, boxSize).withCentre ({ area.getX() + 9.0f, area.getCentreY() });

	drawTickBox (g, button, box.getX(), box.getY(), box.getWidth(), box.getHeight(),
				 button.getToggleState(), button.isEnabled(), shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);

	g.setColour (button.findColour (juce::ToggleButton::textColourId)
				   .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));
	g.setFont (juce::Font (juce::FontOptions (12.5f)));
	g.drawFittedText (button.getButtonText(), area.withTrimmedLeft (box.getRight() + 6.0f).toNearestInt(),
					  juce::Justification::centredLeft, 1, 0.85f);
}

void LightLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
	float sliderPos, float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style, juce::Slider& slider)
{
	if (style == juce::Slider::LinearVertical)
	{
		drawVerticalSlider (g, x, y, width, height, sliderPos, slider);
		return;
	}

	if (style != juce::Slider::LinearHorizontal)
	{
		juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
		return;
	}

	const bool enabled = slider.isEnabled();
	const float centreY = (float) y + (float) height * 0.5f;
	constexpr float trackHeight = 4.0f;
	constexpr float thumbRadius = theme::sliderThumbRadius;	// shared with the loop point marker

	const juce::Rectangle<float> track ((float) x, centreY - trackHeight * 0.5f, (float) width, trackHeight);
	g.setColour (slider.findColour (juce::Slider::backgroundColourId).withMultipliedAlpha (enabled ? 1.0f : 0.6f));
	g.fillRoundedRectangle (track, trackHeight * 0.5f);

	// getSliderLayout() has already inset the slider bounds by the thumb radius, so sliderPos
	// maps directly onto the track (insetting it again would shift the thumb against the mouse)
	const float thumbX = juce::jlimit (track.getX(), track.getRight(), sliderPos);

	if (enabled && thumbX > track.getX() + 1.0f)
	{
		g.setColour (slider.findColour (juce::Slider::trackColourId));
		g.fillRoundedRectangle (track.withRight (thumbX), trackHeight * 0.5f);
	}

	const auto thumb = juce::Rectangle<float> (thumbRadius * 2.0f, thumbRadius * 2.0f).withCentre ({ thumbX, centreY });
	drawSoftShadow (g, thumb, thumbRadius, theme::controlShadow().withMultipliedAlpha (enabled ? 1.0f : 0.45f),
					theme::controlShadowBlur, { 0, 1 });
	g.setColour (slider.findColour (juce::Slider::thumbColourId));
	g.fillEllipse (thumb);
	g.setColour (juce::Colour (0xffc5c5cb));
	g.drawEllipse (thumb, 1.0f);
}

void LightLookAndFeel::drawVerticalSlider (juce::Graphics& g, int x, int y, int width, int height,
										   float sliderPos, juce::Slider& slider)
{
	const bool enabled = slider.isEnabled();
	const float centreX = (float) x + (float) width * 0.5f;
	constexpr float trackWidth = 4.0f;

	const juce::Rectangle<float> track (centreX - trackWidth * 0.5f, (float) y, trackWidth, (float) height);
	g.setColour (slider.findColour (juce::Slider::backgroundColourId).withMultipliedAlpha (enabled ? 1.0f : 0.6f));
	g.fillRoundedRectangle (track, trackWidth * 0.5f);

	// getSliderLayout() has already inset the slider bounds, so sliderPos maps directly onto
	// the track (the filled part grows upwards from the bottom)
	const float thumbY = juce::jlimit (track.getY(), track.getBottom(), sliderPos);

	if (enabled && thumbY < track.getBottom() - 1.0f)
	{
		g.setColour (slider.findColour (juce::Slider::trackColourId));
		g.fillRoundedRectangle (track.withTop (thumbY), trackWidth * 0.5f);
	}

	// a fader cap instead of the round thumb of the horizontal sliders
	const auto cap = juce::Rectangle<float> (juce::jmin ((float) width, 24.0f), 11.0f)
						 .withCentre ({ centreX, thumbY });
	drawSoftShadow (g, cap, 3.0f, theme::controlShadow().withMultipliedAlpha (enabled ? 1.0f : 0.45f),
					theme::controlShadowBlur, { 0, 1 });
	g.setColour (slider.findColour (juce::Slider::thumbColourId));
	g.fillRoundedRectangle (cap, 3.0f);
	g.setColour (juce::Colour (0xffc5c5cb));
	g.drawRoundedRectangle (cap.reduced (0.5f), 3.0f, 1.0f);
	g.fillRect (cap.getX() + 3.0f, cap.getCentreY() - 0.5f, cap.getWidth() - 6.0f, 1.0f);
}

void LightLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
	float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider)
{
	const bool enabled = slider.isEnabled();
	const float diameter = (float) juce::jmin (width, height);
	const auto bounds = juce::Rectangle<float> (diameter, diameter)
							.withCentre ({ (float) x + (float) width * 0.5f,
										   (float) y + (float) height * 0.5f });
	const auto knob = bounds.reduced (2.0f);
	const float radius = knob.getWidth() * 0.5f;
	const float angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

	// a tick above the knob marks the centre position (the pan knob is symmetric)
	g.setColour (theme::secondaryText().withMultipliedAlpha (enabled ? 0.7f : 0.3f));
	g.fillRoundedRectangle (knob.getCentreX() - 0.5f, bounds.getY(), 1.0f, 3.0f, 0.5f);

	drawSoftShadow (g, knob, radius, theme::controlShadow().withMultipliedAlpha (enabled ? 1.0f : 0.45f),
					theme::controlShadowBlur, { 0, 1 });
	g.setColour (juce::Colours::white);
	g.fillEllipse (knob);

	// the accent arc fills up as the knob turns
	const float arcThickness = juce::jmax (2.0f, radius * 0.24f);
	juce::Path arc;
	arc.addCentredArc (knob.getCentreX(), knob.getCentreY(), radius - arcThickness * 0.5f - 0.5f,
					   radius - arcThickness * 0.5f - 0.5f, 0.0f, rotaryStartAngle, angle, true);
	g.setColour (slider.findColour (juce::Slider::trackColourId).withMultipliedAlpha (enabled ? 1.0f : 0.5f));
	g.strokePath (arc, juce::PathStrokeType (arcThickness, juce::PathStrokeType::curved,
											 juce::PathStrokeType::rounded));

	// the pointer
	juce::Path pointer;
	const float pointerThickness = juce::jmax (1.6f, radius * 0.13f);
	pointer.addRoundedRectangle (pointerThickness * -0.5f, -radius + 2.5f,
								 pointerThickness, radius * 0.55f, pointerThickness * 0.5f);
	pointer.applyTransform (juce::AffineTransform::rotation (angle)
								.translated (knob.getCentreX(), knob.getCentreY()));
	g.setColour (theme::primaryText());
	g.fillPath (pointer);

	g.setColour (juce::Colour (0xffc5c5cb));
	g.drawEllipse (knob.reduced (0.5f), 1.0f);
}

void LightLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height,
	bool /*isButtonDown*/, int /*buttonX*/, int /*buttonY*/, int /*buttonW*/, int /*buttonH*/, juce::ComboBox& box)
{
	const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);

	g.setColour (theme::card().withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.5f));
	g.fillRoundedRectangle (bounds, 6.0f);
	g.setColour (theme::border());
	g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);

	juce::Path chevron;
	const float centreX = (float) width - 14.0f;
	const float centreY = (float) height * 0.5f;
	chevron.startNewSubPath (centreX - 4.0f, centreY - 2.0f);
	chevron.lineTo (centreX, centreY + 2.5f);
	chevron.lineTo (centreX + 4.0f, centreY - 2.0f);
	g.setColour (theme::secondaryText());
	g.strokePath (chevron, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font LightLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
	return juce::Font (juce::FontOptions (12.5f));
}

void LightLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
	label.setBounds (10, 1, box.getWidth() - 28, box.getHeight() - 2);
	label.setFont (getComboBoxFont (box));
	label.setJustificationType (juce::Justification::centredLeft);
}

} // namespace vgmplay::ui
