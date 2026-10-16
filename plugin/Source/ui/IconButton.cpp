#include "IconButton.h"

#include "ButtonPaint.h"
#include "Icons.h"
#include "Theme.h"

namespace toybox::ui
{

void IconButton::paintButton (juce::Graphics& g, bool isMouseOverButton, bool isButtonDown)
{
	// The face comes from the shared painter (docs/desgin/ui-button-design.md): a neutral material or the
	// accent/semantic fill, with the soft shadow inside the button's own bounds.
	const bool toggled = getToggleState();
	const bool enabled = isEnabled();
	const auto bounds = getLocalBounds().toFloat();

	ButtonFace face;
	face.level = toggled ? ButtonLevel::filled : ButtonLevel::material;
	face.toggled = toggled;
	face.enabled = enabled;
	face.hovered = isMouseOverButton;
	face.pressed = isButtonDown;
	face.hasFocus = hasKeyboardFocus (false);
	face.fill = findColour (juce::TextButton::buttonOnColourId);

	drawButtonFace (g, bounds, face);

	auto iconColour = toggled ? findColour (juce::TextButton::textColourOnId)
							  : findColour (juce::TextButton::textColourOffId);
	iconColour = iconColour.withMultipliedAlpha (enabled ? 1.0f : theme::buttonDisabledIconAlpha);

	const auto shape = buttonFaceShape (bounds);
	const auto iconArea = shape.reduced (shape.getWidth() * theme::iconInsetRatio,
										 shape.getHeight() * theme::iconInsetRatio);
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
	else if (icon == Icon::sliders)
	{
		drawSlidersGlyph (g, iconArea, iconColour);
	}
	else if (icon == Icon::spreadOutputs)
	{
		drawSpreadOutputsGlyph (g, iconArea, iconColour);
	}
	else if (icon == Icon::mergeToMaster)
	{
		drawMergeToMasterGlyph (g, iconArea, iconColour);
	}
	else
	{
		drawRepeatGlyph (g, iconArea, iconColour);
	}
}

} // namespace toybox::ui
