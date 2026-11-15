#pragma once

#include <juce_graphics/juce_graphics.h>

namespace toybox::ui::channelName
{

/** The three numbers that decide how a strip's name row is arranged. */
constexpr int kMarkSize = 16;		// the type mark, when it fits beside the text
constexpr int kMarkGap = 4;		// its distance from the first letter
constexpr int kTightMark = 14;		// the smaller mark, when the name would not fit beside it
constexpr int kTightGap = 3;

/** Where the mark and the name go inside one strip's name row.

	`mark` is empty when there is none to draw. `justification` and `shrink` are what the caller
	hands to `drawText`, so the two decisions travel with the rectangles instead of being re-derived
	at the call site.

	**Why this is a function and not forty lines of `paint()`.** The arrangement has three shapes -
	mark hugging the name, mark at the edge with a smaller glyph, and no mark at all - and which one
	applies depends on two floating-point comparisons. While that lived in `paint()` the only way to
	know which shape a name took was to render a window and look at it, and the row was rewritten five
	times (once by revert) before it settled. `code-quality.md` §6 names the same failure: layout that
	can only be seen cannot be asserted, so it gets re-litigated.
*/
struct Layout
{
	juce::Rectangle<float> mark;
	juce::Rectangle<float> text;
	juce::Justification justification = juce::Justification::centred;

	/** True when the name did not fit and `text` is the leftover row, to be shrunk by `drawText`. */
	bool shrink = false;
};

/** Arranges `row` for a name `textWidth` wide.

	`textWidth` is measured by the caller: it needs a font, and a font needs the same measurement
	machinery either way, so keeping it out is what makes this callable from a test.

	Three outcomes, in the order they are tried:

	1. no mark (`hasMark == false`) - the name centres across the whole row;
	2. the mark fits beside the name - it hugs the first letter, and the *name* stays centred on the
	   row, because the name is what the eye lines up down the column;
	3. it does not fit - the mark takes the edge at `kTightMark` and the name gets what is left,
	   which is the shape that cannot overflow. Smaller glyph rather than a truncated name: `ADPCM-A 1`
	   cut down to `ADPCM` loses the number telling six sample channels apart.
*/
inline Layout layOut (juce::Rectangle<float> row, float textWidth, bool hasMark)
{
	Layout layout;
	layout.text = row;

	if (! hasMark)
	{
		return layout;
	}

	const float textStart = row.getCentreX () - textWidth * 0.5f;
	const float markX = textStart - (float) kMarkGap - (float) kMarkSize;

	if (textWidth <= row.getWidth () && markX >= row.getX ())
	{
		layout.mark = { markX, row.getCentreY () - (float) kMarkSize * 0.5f,
						(float) kMarkSize, (float) kMarkSize };
		layout.justification = juce::Justification::centredLeft;
		layout.text = { textStart, row.getY (), textWidth + 2.0f, row.getHeight () };
		return layout;
	}

	auto left = row;
	const auto mark = left.removeFromLeft (kTightMark + kTightGap);

	layout.mark = { mark.getX (), mark.getCentreY () - (float) kTightMark * 0.5f,
					(float) kTightMark, (float) kTightMark };
	layout.justification = juce::Justification::centred;
	layout.shrink = true;

	// `removeFromLeft` already took the gap, so the name starts here rather than one gap further
	// on. The code this replaced took it twice - 3 px of mark gap plus another 3 - which left the
	// tight row with a *wider* gap (6) than the roomy one (4). Found by the boundary assertion in
	// UiButtonTest::checkChannelNameLayout, which is the case the eye never catches.
	layout.text = left;

	return layout;
}

} // namespace toybox::ui::channelName
