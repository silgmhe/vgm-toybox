#include "ToyboxLookAndFeel.h"

#include "ButtonPaint.h"
#include "SoftShadow.h"
#include "Theme.h"

namespace toybox::ui
{

ToyboxLookAndFeel::ToyboxLookAndFeel (void)
{
	refresh();
}

void ToyboxLookAndFeel::refresh (void)
{
	// the scheme supplies defaults for everything the UI does not colour itself, so it has to be
	// chosen here: a LookAndFeel_V4 starts out light, which would show through in dark mode
	setColourScheme (theme::current() == theme::Id::dark
						 ? juce::LookAndFeel_V4::getDarkColourScheme()
						 : juce::LookAndFeel_V4::getLightColourScheme());

	setColour (juce::ResizableWindow::backgroundColourId,		theme::window());
	setColour (juce::Label::textColourId,						theme::primaryText());

	setColour (juce::TextButton::buttonColourId,				theme::card());
	setColour (juce::TextButton::buttonOnColourId,				theme::accent());
	setColour (juce::TextButton::textColourOffId,				theme::primaryText());
	setColour (juce::TextButton::textColourOnId,				theme::onAccent());

	setColour (juce::ComboBox::backgroundColourId,				theme::card());
	setColour (juce::ComboBox::textColourId,					theme::primaryText());
	setColour (juce::ComboBox::outlineColourId,					juce::Colours::transparentBlack);
	setColour (juce::ComboBox::arrowColourId,					theme::secondaryText());

	setColour (juce::PopupMenu::backgroundColourId,				theme::card());
	setColour (juce::PopupMenu::textColourId,					theme::primaryText());
	setColour (juce::PopupMenu::highlightedBackgroundColourId,	theme::accent());
	setColour (juce::PopupMenu::highlightedTextColourId,		theme::onAccent());

	setColour (juce::Slider::backgroundColourId,				theme::track());
	setColour (juce::Slider::trackColourId,						theme::accent());
	setColour (juce::Slider::thumbColourId,						theme::controlFaceTop());
	setColour (juce::Slider::textBoxTextColourId,				theme::primaryText());
	// Not transparentBlack: createSliderTextBox() feeds this colour to the editing TextEditor
	// with alpha forced to 1, so a transparent background would become an opaque black box.
	setColour (juce::Slider::textBoxBackgroundColourId,			theme::card());
	setColour (juce::Slider::textBoxOutlineColourId,			juce::Colours::transparentBlack);
	setColour (juce::Slider::textBoxHighlightColourId,			theme::accent().withAlpha (0.25f));

	setColour (juce::ToggleButton::textColourId,				theme::primaryText());
	setColour (juce::ToggleButton::tickColourId,				theme::accent());
	setColour (juce::ToggleButton::tickDisabledColourId,		theme::border());

	// text editors (the slider text boxes when they are being typed into) - the light scheme
	// would be readable by itself, these just keep them on the app's palette
	setColour (juce::TextEditor::backgroundColourId,			theme::card());
	setColour (juce::TextEditor::textColourId,					theme::primaryText());
	setColour (juce::TextEditor::outlineColourId,				theme::border());
	setColour (juce::TextEditor::focusedOutlineColourId,		theme::accent());
	setColour (juce::TextEditor::highlightColourId,				theme::accent().withAlpha (0.25f));
	setColour (juce::TextEditor::highlightedTextColourId,		theme::onAccent());

	setColour (juce::TooltipWindow::backgroundColourId,			theme::tooltipBackground());
	setColour (juce::TooltipWindow::textColourId,				theme::primaryText());
	setColour (juce::TooltipWindow::outlineColourId,			theme::border());

	// The menu bar and its popups take both their background and their text from these PopupMenu
	// ids - that is what drawMenuBarBackground()/drawMenuBarItem() look up - so styling them here
	// is what keeps the whole menu on the palette (they used to be set by the editor, which meant
	// they were only ever set once).
	setColour (juce::PopupMenu::headerTextColourId,				theme::secondaryText());
}

void ToyboxLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
	const juce::Colour& backgroundColour, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
	// Same face as every other button (docs/desgin/ui-button-design.md): the neutral material when
	// the caller's colour is the theme's neutral one (the mixer's M/S buttons keep it while they
	// are off), the filled variant when the button carries a colour of its own (accent, mute red,
	// solo amber). Which one also decides the hairline and the shadow - both live in the painter.
	//
	// "Neutral" is a comparison against the theme's own neutrals, not a brightness test. Brightness
	// happened to work while there was only a light palette (white face = bright, accent = dark),
	// but the dark palette's neutral is #2c313a - darker than its #61afef accent - so brightness
	// would call every neutral button "filled" and paint it in the card colour, i.e. invisible.
	const bool filled = (backgroundColour != theme::card() && backgroundColour != theme::buttonMaterial());

	ButtonFace face;
	face.level = filled ? ButtonLevel::filled : ButtonLevel::material;
	face.toggled = button.getToggleState();
	face.enabled = button.isEnabled();
	face.hovered = shouldDrawButtonAsHighlighted;
	face.pressed = shouldDrawButtonAsDown;
	face.hasFocus = button.hasKeyboardFocus (false);
	face.fill = backgroundColour;

	drawButtonFace (g, button.getLocalBounds().toFloat(), face);
}

void ToyboxLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button,
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

void ToyboxLookAndFeel::drawTickBox (juce::Graphics& g, juce::Component& component,
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
		g.setColour (theme::onAccent());
		g.strokePath (tick, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
	}
	else
	{
		g.setColour (theme::controlFaceTop());
		g.fillRoundedRectangle (box, radius);
		g.setColour (shouldDrawButtonAsHighlighted ? theme::secondaryText().withAlpha (0.6f) : theme::controlEdge());
		g.drawRoundedRectangle (box.reduced (0.5f), radius, 1.0f);
	}
}

void ToyboxLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
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

void ToyboxLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
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
	g.setColour (theme::controlEdge());
	g.drawEllipse (thumb, 1.0f);
}

void ToyboxLookAndFeel::drawVerticalSlider (juce::Graphics& g, int x, int y, int width, int height,
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

	// A fader cap instead of the round thumb of the horizontal sliders: the small square knob
	// of a real console fader, with a slight bevel and the groove a finger grips it by.
	constexpr float capRadius = 3.5f;
	const float capSize = juce::jmin (juce::jmin ((float) width, (float) height) * 0.45f, 15.0f);
	const auto cap = juce::Rectangle<float> (capSize, capSize).withCentre ({ centreX, thumbY });

	drawSoftShadow (g, cap, capRadius, theme::controlShadow().withMultipliedAlpha (enabled ? 1.0f : 0.45f),
					theme::controlShadowBlur, { 0, 1 });
	g.setGradientFill (juce::ColourGradient (theme::controlFaceTop(), cap.getCentreX(), cap.getY(),
											 theme::controlFaceBottom(), cap.getCentreX(), cap.getBottom(), false));
	g.fillRoundedRectangle (cap, capRadius);
	g.setColour (theme::controlEdge());
	g.drawRoundedRectangle (cap.reduced (0.5f), capRadius, 1.0f);

	const float grooveInset = capRadius + 1.0f;
	g.setColour (theme::controlGroove());
	g.fillRect (cap.getX() + grooveInset, cap.getCentreY() - 1.0f,
				cap.getWidth() - 2.0f * grooveInset, 2.0f);
	g.setColour (theme::controlGrooveHighlight());
	g.fillRect (cap.getX() + grooveInset, cap.getCentreY() + 1.0f,
				cap.getWidth() - 2.0f * grooveInset, 1.0f);
}

void ToyboxLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
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
	g.setColour (theme::controlFaceTop());
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

	g.setColour (theme::controlEdge());
	g.drawEllipse (knob.reduced (0.5f), 1.0f);
}

void ToyboxLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height,
	bool isButtonDown, int /*buttonX*/, int /*buttonY*/, int /*buttonW*/, int /*buttonH*/, juce::ComboBox& box)
{
	// A combo box is a button with a chevron on it: same material, same shadow, same radius rule
	// (the hard-coded 6.0f this used to have was one of the three radius rules the design doc set
	// out to remove).
	ButtonFace face;
	face.level = ButtonLevel::material;
	face.enabled = box.isEnabled();
	face.pressed = isButtonDown;
	face.hasFocus = box.hasKeyboardFocus (false);
	face.fill = theme::card();

	drawButtonFace (g, juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height), face);

	juce::Path chevron;
	const float centreX = (float) width - 14.0f;
	const float centreY = (float) height * 0.5f;
	chevron.startNewSubPath (centreX - 4.0f, centreY - 2.0f);
	chevron.lineTo (centreX, centreY + 2.5f);
	chevron.lineTo (centreX + 4.0f, centreY - 2.0f);
	g.setColour (theme::secondaryText().withMultipliedAlpha (box.isEnabled() ? 1.0f : theme::buttonDisabledIconAlpha));
	g.strokePath (chevron, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font ToyboxLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
	return juce::Font (juce::FontOptions (12.5f));
}

void ToyboxLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
	label.setBounds (10, 1, box.getWidth() - 28, box.getHeight() - 2);
	label.setFont (getComboBoxFont (box));
	label.setJustificationType (juce::Justification::centredLeft);
}

} // namespace toybox::ui
