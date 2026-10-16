#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/** Shared colours and metrics of the plugin GUI.

	Everything the UI draws is derived from these, so the palette and the card/button metrics
	have exactly one definition. The light look is macOS-flavoured: white cards on a light grey
	window, hairline borders, a blue accent for anything that is "on".
*/
namespace toybox::ui::theme
{

// --- palette ------------------------------------------------------------------------
inline juce::Colour window (void)		{ return juce::Colour (0xfff2f2f7); }
inline juce::Colour windowTop (void)	{ return juce::Colour (0xfff9f9fc); }
inline juce::Colour windowBottom (void)	{ return juce::Colour (0xffecedf2); }
inline juce::Colour card (void)			{ return juce::Colour (0xffffffff); }
inline juce::Colour accent (void)		{ return juce::Colour (0xff0a84ff); }
inline juce::Colour primaryText (void)	{ return juce::Colour (0xff1d1d1f); }
inline juce::Colour secondaryText (void){ return juce::Colour (0xff74747a); }
inline juce::Colour border (void)		{ return juce::Colour (0xffd7d7dd); }
inline juce::Colour cardBorder (void)	{ return juce::Colour (0xffe6e6ec); }
inline juce::Colour track (void)		{ return juce::Colour (0xffdcdce2); }
inline juce::Colour error (void)		{ return juce::Colour (0xffd70015); }

// the mixer's mute (red) and solo (amber) buttons follow the DAW convention
inline juce::Colour mute (void)			{ return juce::Colour (0xffe5484d); }
inline juce::Colour solo (void)			{ return juce::Colour (0xfff5c518); }

// the meter gradient: quiet is green, loud turns amber, 0 dB and above are red
inline juce::Colour meterLow (void)		{ return juce::Colour (0xff34c759); }
inline juce::Colour meterMid (void)		{ return juce::Colour (0xffffcc00); }
inline juce::Colour meterHigh (void)	{ return juce::Colour (0xffff3b30); }

// --- metrics ------------------------------------------------------------------------
inline constexpr int margin = 14;			// window border
inline constexpr int cardGap = 10;			// gap between two cards (or a card and the top row)
inline constexpr int cardPadding = 12;		// padding inside a card
inline constexpr int topRowHeight = 32;		// open button + status line
inline constexpr int rowGap = 8;

inline constexpr float cardRadius = 10.0f;
inline constexpr float buttonRadius = 8.0f;

/** Radius of the position slider thumb. LightLookAndFeel draws the thumb, TransportPanel
	draws the loop point marker on top of the track - both have to use the same value. */
inline constexpr float sliderThumbRadius = 6.5f;

// --- elevation ----------------------------------------------------------------------
// Everything that floats above the window casts a soft, blurred shadow: the cards a
// two-layer one (a wide "key" light below the shape plus a tight "ambient" contact
// shadow), filled accent controls a coloured glow. All of them are just a few percent
// opaque, so the depth stays subtle.
inline juce::Colour cardAmbientShadow (void)	{ return juce::Colour (0x14000000); }
inline juce::Colour cardKeyShadow (void)		{ return juce::Colour (0x11000000); }
inline constexpr int cardAmbientBlur = 3;
inline constexpr int cardKeyBlur = 16;
inline constexpr int cardKeyOffsetY = 5;

inline juce::Colour accentGlow (void)			{ return juce::Colour (0x4d0a84ff); }
inline constexpr int accentGlowBlur = 12;
inline constexpr int accentGlowOffsetY = 3;

// slider thumbs and ticked checkboxes (their shadow fits inside their own bounds)
inline juce::Colour controlShadow (void)		{ return juce::Colour (0x38000000); }
inline constexpr int controlShadowBlur = 5;

// --- buttons (docs/ui-button-design.md) -----------------------------------------------
// One language for every button: a neutral white face with a hairline, or a filled accent /
// semantic colour, and **one** soft shadow that makes it read as floating above the card.
// The shadow is painted inside the button's own bounds - the visible face is inset by
// `buttonShadowInset` (see ButtonPaint.h) - so no parent has to know about it. One layer, flat
// material, no gradient on purpose: a juce::DropShadow blurs an image per call, so every extra
// layer costs the GPU again.
inline juce::Colour buttonMaterial (void)	{ return juce::Colour (0xffffffff); }
inline juce::Colour buttonHairline (void)	{ return juce::Colour (0xffd0d0d8); }
inline juce::Colour buttonShadow (void)		{ return juce::Colour (0x26000000); }
inline constexpr int buttonShadowBlur = 3;
inline constexpr int buttonShadowOffsetY = 1;
inline constexpr int buttonShadowInset = 4;		// room the face reserves for its shadow

inline constexpr float buttonHoverDarken = 0.04f;
inline constexpr float buttonPressedDarken = 0.08f;
inline constexpr int buttonPressedBlurShift = -2;	// pressing shrinks the shadow (physical cue)
inline constexpr int buttonPressedOffsetShift = -1;
inline constexpr float buttonDisabledAlpha = 0.45f;
inline constexpr float buttonDisabledIconAlpha = 0.40f;

inline juce::Colour focusRing (void)		{ return juce::Colour (0x660a84ff); }
inline constexpr float focusRingThickness = 2.0f;

inline constexpr float iconInsetRatio = 0.28f;	// glyph area = face inset by this much

/** Corner radius of a button of the given height - smaller controls are less rounded. This is
	the one radius rule; ComboBox, TextButton and IconButton all go through it. */
inline float buttonRadiusFor (float height)	{ return juce::jmin (height * 0.2f, buttonRadius); }

} // namespace toybox::ui::theme
