#include "Theme.h"

namespace toybox::ui::theme
{

/** The light palette. Every one of these was a literal in the getters in Theme.h before there was
	a second palette, and the values are unchanged on purpose: the light look is the one the
	existing screenshots, the pixel test (Tests/UiButtonTest.cpp) and the design doc
	(docs/desgin/ui-button-design.md) describe, so a new mechanism must not move a single pixel.
*/
static const Palette kLight
{
	/* window            */ juce::Colour (0xfff2f2f7),
	/* windowTop         */ juce::Colour (0xfff9f9fc),
	/* windowBottom      */ juce::Colour (0xffecedf2),
	/* card              */ juce::Colour (0xffffffff),
	/* accent            */ juce::Colour (0xff0a84ff),
	/* onAccent          */ juce::Colour (0xffffffff),
	/* primaryText       */ juce::Colour (0xff1d1d1f),
	/* secondaryText     */ juce::Colour (0xff74747a),
	/* border            */ juce::Colour (0xffd7d7dd),
	/* cardBorder        */ juce::Colour (0xffe6e6ec),
	/* track             */ juce::Colour (0xffdcdce2),
	/* error             */ juce::Colour (0xffd70015),
	/* mute              */ juce::Colour (0xffe5484d),
	/* solo              */ juce::Colour (0xfff5c518),
	/* meterLow          */ juce::Colour (0xff34c759),
	/* meterMid          */ juce::Colour (0xffffcc00),
	/* meterHigh         */ juce::Colour (0xffff3b30),
	/* cardAmbientShadow */ juce::Colour (0x14000000),
	/* cardKeyShadow     */ juce::Colour (0x11000000),
	/* accentGlow        */ juce::Colour (0x4d0a84ff),
	/* controlShadow     */ juce::Colour (0x38000000),
	/* buttonMaterial    */ juce::Colour (0xffffffff),
	/* buttonHairline    */ juce::Colour (0xffd0d0d8),
	/* buttonShadow      */ juce::Colour (0x26000000),
	/* focusRing         */ juce::Colour (0x660a84ff),
	/* controlFaceTop    */ juce::Colour (0xffffffff),
	/* controlFaceBottom */ juce::Colour (0xffe6e6ec),
	/* controlEdge       */ juce::Colour (0xffc5c5cb),
	/* controlGroove     */ juce::Colour (0xff8e8e96),
	/* groupFrame        */ juce::Colour (0xffc9c9d2),
	/* tooltipBackground */ juce::Colour (0xf7ffffff)
};

/** The dark palette, after One Dark.

	Two things are not a straight translation of the light one:

	- the depth direction. In light a card is white on a light grey window, so "raised" means
	  "brighter". Here the window is #21252b and the card #2c313a, and every part of the window
	  gradient stays below the card, otherwise a card on the upper half of the window would
	  disappear into it. The shadows get more alpha for the same reason: a shadow only reads
	  against something lighter than itself.
	- a lighter accent. One Dark's blue (#61afef) is made to be *text* on a dark background, so
	  white text on a fill of it would be barely readable - `onAccent` is the near-black the
	  window is made of instead, which is the same trick the light palette uses in reverse.

	The mixer keeps the DAW convention (red mute, amber solo, green/amber/red meter), only
	desaturated towards the One Dark tones. docs/desgin/settings-and-theme-design.md §5 has the
	table this came from, §13 the two values that changed away from it.
*/
static const Palette kDark
{
	/* window            */ juce::Colour (0xff21252b),
	/* windowTop         */ juce::Colour (0xff262a32),
	/* windowBottom      */ juce::Colour (0xff1a1e24),
	/* card              */ juce::Colour (0xff2c313a),
	/* accent            */ juce::Colour (0xff61afef),
	/* onAccent          */ juce::Colour (0xff1b1f26),
	/* primaryText       */ juce::Colour (0xffabb2bf),
	/* secondaryText     */ juce::Colour (0xff7f848e),
	/* border            */ juce::Colour (0xff181a1f),
	/* cardBorder        */ juce::Colour (0xff3e4451),
	/* track             */ juce::Colour (0xff3e4451),
	/* error             */ juce::Colour (0xffe06c75),
	/* mute              */ juce::Colour (0xffe06c75),
	/* solo              */ juce::Colour (0xffe5c07b),
	/* meterLow          */ juce::Colour (0xff98c379),
	/* meterMid          */ juce::Colour (0xffe5c07b),
	/* meterHigh         */ juce::Colour (0xffe06c75),
	/* cardAmbientShadow */ juce::Colour (0x33000000),
	/* cardKeyShadow     */ juce::Colour (0x26000000),
	/* accentGlow        */ juce::Colour (0x4d61afef),
	/* controlShadow     */ juce::Colour (0x4d000000),
	/* buttonMaterial    */ juce::Colour (0xff353b45),
	/* buttonHairline    */ juce::Colour (0xff4b5263),
	/* buttonShadow      */ juce::Colour (0x40000000),
	/* focusRing         */ juce::Colour (0x6661afef),
	/* controlFaceTop    */ juce::Colour (0xff4b5263),
	/* controlFaceBottom */ juce::Colour (0xff373d47),
	/* controlEdge       */ juce::Colour (0xff20242b),
	/* controlGroove     */ juce::Colour (0xff20242b),
	/* groupFrame        */ juce::Colour (0xff454b57),
	/* tooltipBackground */ juce::Colour (0xf72c313a)
};

/** Message thread only (see setTheme), so this needs no lock: nothing else reads it. */
static Id currentTheme = Id::light;

const Palette& palette (void)
{
	return (currentTheme == Id::dark) ? kDark : kLight;
}

Id current (void)
{
	return currentTheme;
}

void setTheme (Id id)
{
	currentTheme = id;
}

Id systemTheme (void)
{
	return juce::Desktop::getInstance().isDarkModeActive() ? Id::dark : Id::light;
}

} // namespace toybox::ui::theme
