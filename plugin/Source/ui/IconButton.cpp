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

	// A captioned button has to fit the glyph and a word into one face: it gives the pair more of
	// the button than a glyph alone takes, and the glyph in turn gives up the bottom 8 px for the
	// caption. Without a caption this is the plain 0.28 inset every other icon button uses.
	constexpr float kCaptionHeight = 8.0f;
	const bool captioned = caption.isNotEmpty();
	const float inset = captioned ? 0.20f : theme::iconInsetRatio;
	auto glyphBox = shape.reduced (shape.getWidth() * inset, shape.getHeight() * inset);

	const auto captionArea = captioned ? glyphBox.removeFromBottom (kCaptionHeight).toNearestInt()
									   : juce::Rectangle<int>();
	const auto glyphArea = glyphBox;

	g.setColour (iconColour);

	if (icon == Icon::play)
	{
		juce::Path path;
		path.addTriangle (glyphArea.getX(), glyphArea.getY(),
						  glyphArea.getX(), glyphArea.getBottom(),
						  glyphArea.getRight(), glyphArea.getCentreY());
		g.fillPath (path);
	}
	else if (icon == Icon::pause)
	{
		juce::Path path;
		const float barWidth = glyphArea.getWidth() * 0.32f;
		path.addRectangle (glyphArea.getX(), glyphArea.getY(), barWidth, glyphArea.getHeight());
		path.addRectangle (glyphArea.getRight() - barWidth, glyphArea.getY(), barWidth, glyphArea.getHeight());
		g.fillPath (path);
	}
	else if (icon == Icon::stop)
	{
		g.fillRoundedRectangle (glyphArea, 1.5f);
	}
	else if (icon == Icon::playStart)
	{
		drawPlayStartGlyph (g, glyphArea, iconColour);
	}
	else if (icon == Icon::sliders)
	{
		drawSlidersGlyph (g, glyphArea, iconColour);
	}
	else if (icon == Icon::spreadOutputs)
	{
		drawSpreadOutputsGlyph (g, glyphArea, iconColour);
	}
	else if (icon == Icon::mergeToMaster)
	{
		drawMergeToMasterGlyph (g, glyphArea, iconColour);
	}
	else if (icon == Icon::sync)
	{
		drawSyncGlyph (g, glyphArea, iconColour);
	}
	else if (icon == Icon::cue)
	{
		drawCueGlyph (g, glyphArea, iconColour);
	}
	else
	{
		drawRepeatGlyph (g, glyphArea, iconColour);
	}

	if (captioned)
	{
		g.setFont (juce::Font (juce::FontOptions (7.0f, juce::Font::bold)));
		g.drawText (caption, captionArea, juce::Justification::centred, false);
	}
}

} // namespace toybox::ui
