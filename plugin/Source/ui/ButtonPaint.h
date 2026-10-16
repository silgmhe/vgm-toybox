#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "SoftShadow.h"
#include "Theme.h"

namespace toybox::ui
{

/** Where a button sits in the hierarchy (docs/ui-button-design.md §6).

	`material` is the default: the neutral white face with a hairline and a soft shadow.
	`filled` is the prominent one (accent, mute red, solo amber): the caller supplies the colour,
	and a toggled one additionally gets the coloured glow, which the surrounding card paints. */
enum class ButtonLevel
{
	material,
	filled
};

/** Everything that changes how a button face is drawn. */
struct ButtonFace
{
	ButtonLevel level = ButtonLevel::material;
	bool toggled = false;
	bool enabled = true;
	bool hovered = false;
	bool pressed = false;
	bool hasFocus = false;
	juce::Colour fill { theme::card() };// only used by ButtonLevel::filled
};

/** How much of the button's own bounds the shadow needs (see buttonFaceShape()). Kept small for
	small controls, so a 24 px button does not spend a third of its face on a shadow. */
inline float buttonInsetFor (juce::Rectangle<float> bounds)
{
	return juce::jmin ((float) theme::buttonShadowInset, bounds.getHeight() * 0.15f);
}

/** The visible face of a button inside its own bounds.

	JUCE clips every component to its own bounds, so a button that paints its own shadow has to
	reserve room for it - this is that reservation. It also means no parent ever has to know about
	a button's shadow, which is what keeps new buttons free of extra wiring. */
inline juce::Rectangle<float> buttonFaceShape (juce::Rectangle<float> bounds)
{
	return bounds.reduced (buttonInsetFor (bounds));
}

/** Paints a button face: one soft shadow, the fill, a hairline and the focus ring.

	Deliberately one shadow layer, flat material, no gradient - a juce::DropShadow renders (and
	blurs) an image per call, so every layer costs the GPU twice over (see the design doc §4.4). */
inline void drawButtonFace (juce::Graphics& g, juce::Rectangle<float> bounds, const ButtonFace& face)
{
	const auto shape = buttonFaceShape (bounds);
	if (shape.getWidth() <= 1.0f || shape.getHeight() <= 1.0f)
	{
		return;
	}

	const float radius = theme::buttonRadiusFor (shape.getHeight());
	const bool material = (face.level == ButtonLevel::material);

	// the shadow: what makes the button read as floating above the card. Pressing shrinks it,
	// which is the cue that the button moved down (it is not just a darker fill).
	const int blur = juce::jmax (1, theme::buttonShadowBlur + (face.pressed ? theme::buttonPressedBlurShift : 0));
	const int offsetY = theme::buttonShadowOffsetY + (face.pressed ? theme::buttonPressedOffsetShift : 0);
	const float shadowAlpha = face.enabled ? 1.0f : theme::buttonDisabledAlpha;
	drawSoftShadow (g, shape, radius, theme::buttonShadow().withMultipliedAlpha (shadowAlpha),
					blur, { 0, offsetY });

	// the fill: the neutral material or the caller's colour (accent / mute / solo)
	const float darken = face.pressed ? theme::buttonPressedDarken
									  : (face.hovered ? theme::buttonHoverDarken : 0.0f);
	auto fill = material ? theme::buttonMaterial() : face.fill;
	if (! face.enabled)
	{
		// a disabled button should read as inactive on a white card, not just have a dimmer icon
		fill = fill.interpolatedWith (theme::track(), 0.55f);
	}
	g.setColour (fill.darker (darken));
	g.fillRoundedRectangle (shape, radius);

	// the hairline keeps a light button readable on a light card; a filled button has its own
	// contrast and goes without
	if (material)
	{
		g.setColour (theme::buttonHairline().withMultipliedAlpha (face.enabled ? 1.0f : theme::buttonDisabledAlpha));
		g.drawRoundedRectangle (shape.reduced (0.5f), radius, 1.0f);
	}

	// the focus ring (the UI had none, so keyboard users could not see where they were)
	if (face.hasFocus)
	{
		g.setColour (theme::focusRing());
		g.drawRoundedRectangle (shape.expanded (1.0f), radius + 1.0f, theme::focusRingThickness);
	}
}

/** Paints the coloured glow of a toggled prominent button.

	The surrounding **container** calls this - the card from its paintCardContent(), the mixer
	strip from its paint() - because the glow's blur reaches considerably further than the button's
	own inset, so the button cannot paint it itself. Pass the colour the button is filled with
	(`theme::muteGlow()` for mute, `theme::soloGlow()` for solo, the default for the accent ones).
	The container must also repaint that area when the toggle state changes; `buttonGlowBounds()`
	gives the rectangle to pass to repaint(). */
inline void drawButtonGlow (juce::Graphics& g, juce::Rectangle<int> buttonBounds,
							juce::Colour colour = theme::accentGlow())
{
	const auto shape = buttonFaceShape (buttonBounds.toFloat());
	if (shape.getWidth() <= 1.0f || shape.getHeight() <= 1.0f)
	{
		return;
	}

	drawSoftShadow (g, shape, theme::buttonRadiusFor (shape.getHeight()), colour,
					theme::accentGlowBlur, { 0, theme::accentGlowOffsetY });
}

/** The area a button's glow can touch - repaint this when the toggle state flips. */
inline juce::Rectangle<int> buttonGlowBounds (juce::Rectangle<int> buttonBounds)
{
	const int reach = theme::accentGlowBlur + theme::accentGlowOffsetY + 2;
	return buttonBounds.expanded (reach);
}

} // namespace toybox::ui
