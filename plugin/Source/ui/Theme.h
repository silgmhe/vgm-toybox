#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/** Shared colours and metrics of the plugin GUI.

	Everything the UI draws is derived from these, so the palette and the card/button metrics
	have exactly one definition. The light palette is macOS-flavoured: white cards on a light grey
	window, hairline borders, a blue accent for anything that is "on". The dark one follows One
	Dark, and there the cards are the *lighter* surface - a raised card reads as raised only
	because it is brighter than what is behind it, so dark has to turn that relationship around
	instead of dimming it (docs/desgin/settings-and-theme-design.md §5).

	Every getter reports the palette that is in force, so no call site has to know which theme is
	on: switching is `setTheme()`, and whatever repaints itself afterwards draws in the new
	colours without being touched.
*/
namespace toybox::ui::theme
{

// --- palette ------------------------------------------------------------------------

enum class Id { light, dark };

/** One colour per role the UI draws in; the two palettes below fill it in.

	The roles are named after what they are painted on rather than after how they look, because
	that is the part the two palettes agree on: a "window" is the back surface, a "card" floats
	above it, a "track" is the groove a slider runs in.
*/
struct Palette
{
	// chrome
	juce::Colour window, windowTop, windowBottom, card;	// top/bottom are the gradient's ends
	juce::Colour accent;								// anything that is "on"
	juce::Colour onAccent;								// text and glyphs sitting on an accent fill
	juce::Colour primaryText, secondaryText;
	juce::Colour border, cardBorder, track, error;

	// the mixer's mute (red) and solo (amber) buttons follow the DAW convention in both palettes
	juce::Colour mute, solo;

	// the meter gradient: quiet is green, loud turns amber, 0 dB and above are red
	juce::Colour meterLow, meterMid, meterHigh;

	// elevation
	juce::Colour cardAmbientShadow, cardKeyShadow, accentGlow, controlShadow;

	// buttons (docs/desgin/ui-button-design.md)
	juce::Colour buttonMaterial, buttonHairline, buttonShadow, focusRing;

	// The "metal" controls: slider thumbs, the mixer's fader caps, the rotary knob and tick boxes.
	// Light draws them white with a grey edge; dark needs a grey cap, because a white one on a
	// dark card reads as a hole rather than as something standing up.
	juce::Colour controlFaceTop, controlFaceBottom, controlEdge, controlGroove;

	// A group's dashed frame: the quietest line that still closes a group (TransportPanel's
	// DAW-sync cluster). It has no depth role - it is not a surface, not a border of anything
	// raised or sunk - so it needs its own two values rather than borrowing `border` (dark in
	// both palettes) or `cardBorder` (invisible on a white card).
	juce::Colour groupFrame;

	// Tooltips float over everything, so they are not card-coloured in either palette.
	juce::Colour tooltipBackground;
};

/** The palette in force. Both are static, so this only ever picks one. */
const Palette& palette (void);

/** Which one that is - the settings store decides, see settings/SettingsStore.h. */
Id current (void);

/** Picks the palette. Message thread only, and it deliberately tells nobody: a theme change is
	broadcast by the settings store, which is what the UI listens to. */
void setTheme (Id id);

/** What "follow system" means right now: the desktop's own light/dark preference. It lives here
	and not in the settings store because it is a desktop question rather than a stored value -
	the store knows the three-way choice, the UI layer knows how to ask the system about it. */
Id systemTheme (void);

// One function per role, so a call site says `theme::card()` and never asks which theme is on.
inline juce::Colour window (void)			{ return palette().window; }
inline juce::Colour windowTop (void)		{ return palette().windowTop; }
inline juce::Colour windowBottom (void)		{ return palette().windowBottom; }
inline juce::Colour card (void)				{ return palette().card; }
inline juce::Colour accent (void)			{ return palette().accent; }
inline juce::Colour onAccent (void)			{ return palette().onAccent; }
inline juce::Colour primaryText (void)		{ return palette().primaryText; }
inline juce::Colour secondaryText (void)	{ return palette().secondaryText; }
inline juce::Colour border (void)			{ return palette().border; }
inline juce::Colour cardBorder (void)		{ return palette().cardBorder; }
inline juce::Colour track (void)			{ return palette().track; }
inline juce::Colour error (void)			{ return palette().error; }

inline juce::Colour mute (void)				{ return palette().mute; }
inline juce::Colour solo (void)				{ return palette().solo; }

inline juce::Colour meterLow (void)			{ return palette().meterLow; }
inline juce::Colour meterMid (void)			{ return palette().meterMid; }
inline juce::Colour meterHigh (void)		{ return palette().meterHigh; }

// --- metrics ------------------------------------------------------------------------
inline constexpr int margin = 14;			// window border
inline constexpr int cardGap = 10;			// gap between two cards (or a card and the top row)
inline constexpr int cardPadding = 12;		// padding inside a card
inline constexpr int menuBarHeight = 24;	// the application menu above everything else
inline constexpr int topRowHeight = 36;		// open button + status line (button face = 32)
inline constexpr int rowGap = 8;

inline constexpr float cardRadius = 10.0f;
inline constexpr float buttonRadius = 8.0f;

/** Radius of the position slider thumb. ToyboxLookAndFeel draws the thumb, TransportPanel
	draws the loop point marker on top of the track - both have to use the same value. */
inline constexpr float sliderThumbRadius = 6.5f;

// --- elevation ----------------------------------------------------------------------
// Everything that floats above the window casts a soft, blurred shadow: the cards a
// two-layer one (a wide "key" light below the shape plus a tight "ambient" contact
// shadow), filled accent controls a coloured glow. All of them are just a few percent
// opaque, so the depth stays subtle - and all of them are darker than the surface they fall
// on, which is why the dark palette needs more alpha for the same effect.
inline juce::Colour cardAmbientShadow (void)	{ return palette().cardAmbientShadow; }
inline juce::Colour cardKeyShadow (void)		{ return palette().cardKeyShadow; }
inline constexpr int cardAmbientBlur = 3;
inline constexpr int cardKeyBlur = 16;
inline constexpr int cardKeyOffsetY = 5;

inline juce::Colour accentGlow (void)			{ return palette().accentGlow; }
inline constexpr int accentGlowBlur = 12;
inline constexpr int accentGlowOffsetY = 3;

// The same glow in a semantic colour: a toggled mute or solo button glows red or amber instead of
// accent blue, so its state reads before the fill is even looked at.
inline juce::Colour muteGlow (void)			{ return mute().withAlpha (0.42f); }
inline juce::Colour soloGlow (void)			{ return solo().withAlpha (0.42f); }

// A glow that has to fade out inside a narrow container. The mixer's M/S buttons sit six pixels
// from their strip's edge, so a 12 px blur is cut off there and reads as a rectangle around the
// button; 5 px reaches zero inside the lane and still reads as a halo.
inline constexpr int compactGlowBlur = 5;

// slider thumbs and ticked checkboxes (their shadow fits inside their own bounds)
inline juce::Colour controlShadow (void)		{ return palette().controlShadow; }
inline constexpr int controlShadowBlur = 5;

// --- buttons (docs/desgin/ui-button-design.md) -----------------------------------------------
// One language for every button: a neutral face with a hairline, or a filled accent /
// semantic colour, and **one** soft shadow that makes it read as floating above the card.
// The shadow is painted inside the button's own bounds - the visible face is inset by
// `buttonShadowInset` (see ButtonPaint.h) - so no parent has to know about it. One layer, flat
// material, no gradient on purpose: a juce::DropShadow blurs an image per call, so every extra
// layer costs the GPU again.
inline juce::Colour buttonMaterial (void)	{ return palette().buttonMaterial; }
inline juce::Colour buttonHairline (void)	{ return palette().buttonHairline; }
inline juce::Colour buttonShadow (void)		{ return palette().buttonShadow; }
inline constexpr int buttonShadowBlur = 2;		// tight: the face can only spare a little room
inline constexpr int buttonShadowOffsetY = 1;
// The room the face reserves for its shadow. Kept to 2 px: the shadow is small, and every pixel
// spent here comes off the button (the layouts hand out `face + 2 * inset`, see docs §8).
inline constexpr int buttonShadowInset = 2;

inline constexpr float buttonHoverDarken = 0.04f;
inline constexpr float buttonPressedDarken = 0.08f;
inline constexpr int buttonPressedBlurShift = -2;	// pressing shrinks the shadow (physical cue)
inline constexpr int buttonPressedOffsetShift = -1;
inline constexpr float buttonDisabledAlpha = 0.45f;
inline constexpr float buttonDisabledIconAlpha = 0.40f;

inline juce::Colour focusRing (void)		{ return palette().focusRing; }
inline constexpr float focusRingThickness = 2.0f;

inline constexpr float iconInsetRatio = 0.28f;	// glyph area = face inset by this much

// --- metal controls and hints -------------------------------------------------------
// The thumbs, caps and boxes the palette describes under controlFace*/controlEdge/controlGroove.
inline juce::Colour controlFaceTop (void)		{ return palette().controlFaceTop; }
inline juce::Colour controlFaceBottom (void)	{ return palette().controlFaceBottom; }
inline juce::Colour controlEdge (void)			{ return palette().controlEdge; }
inline juce::Colour controlGroove (void)		{ return palette().controlGroove; }

/** The light line under the groove of a fader cap, so the groove reads as cut into the cap. */
inline juce::Colour controlGrooveHighlight (void)	{ return controlFaceTop().withAlpha (0.85f); }

/** The dashed frame around a group of controls. Not derivable from the card colour in the same
	way for both palettes, which is why the two values are stated instead of being derived. */
inline juce::Colour groupFrame (void)		{ return palette().groupFrame; }

inline juce::Colour tooltipBackground (void)	{ return palette().tooltipBackground; }

/** Corner radius of a button of the given height - smaller controls are less rounded. This is
	the one radius rule; ComboBox, TextButton and IconButton all go through it. */
inline float buttonRadiusFor (float height)	{ return juce::jmin (height * 0.2f, buttonRadius); }

} // namespace toybox::ui::theme
