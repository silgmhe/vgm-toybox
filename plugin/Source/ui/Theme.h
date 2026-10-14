#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

/** Shared colours and metrics of the plugin GUI.

	Everything the UI draws is derived from these, so the palette and the card/button metrics
	have exactly one definition. The light look is macOS-flavoured: white cards on a light grey
	window, hairline borders, a blue accent for anything that is "on".
*/
namespace vgmplay::ui::theme
{

// --- palette ------------------------------------------------------------------------
inline juce::Colour window (void)		{ return juce::Colour (0xfff2f2f7); }
inline juce::Colour card (void)			{ return juce::Colour (0xffffffff); }
inline juce::Colour accent (void)		{ return juce::Colour (0xff0a84ff); }
inline juce::Colour primaryText (void)	{ return juce::Colour (0xff1d1d1f); }
inline juce::Colour secondaryText (void){ return juce::Colour (0xff74747a); }
inline juce::Colour border (void)		{ return juce::Colour (0xffd7d7dd); }
inline juce::Colour track (void)		{ return juce::Colour (0xffdcdce2); }
inline juce::Colour error (void)		{ return juce::Colour (0xffd70015); }

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

} // namespace vgmplay::ui::theme
