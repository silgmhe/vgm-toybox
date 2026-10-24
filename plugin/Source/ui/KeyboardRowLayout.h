#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <string>
#include <vector>

namespace toybox::ui::keyboard
{

/** The measurements a keyboard row is built from.

	These were constants inside the panel, which meant the only way to find out whether a row added
	up - and whether the columns overlapped - was to draw thirteen of them and look. They are a
	value now, so a test can hand in a width and ask what the columns came out as.
*/
struct RowMetrics
{
	int cardInset = 20;			// the card's margin inside the column
	int rowHeight = 48;			// tall enough that the keyboard sits inside the card
	int groupHeader = 26;
	int afterHeader = 2;		// breathing room under a chip's title
	int groupGap = 10;
	int rowGap = 2;

	int rowInsetLeft = 14;		// where the columns start inside the card
	int rowInsetRight = 14;
	int nameWidth = 72;
	int buttonSize = 28;		// the mixer's button height, so the two views agree
	int buttonGap = 6;
	int noteWidth = 56;
	int waveWidth = 88;
	int waveHeight = 26;
	int keyHeight = 32;
	int meterWidth = 96;
	int meterHeight = 20;
	int meterGap = 18;

	// The gaps between the columns, in drawing order.
	int nameToButton = 6;
	int noteToWave = 8;
	int waveToKeys = 10;
	int buttonToNote = 10;
};

/** One row of the keyboard: either a chip's title, or a channel's five columns. */
struct Row
{
	bool header = false;
	juce::String title;
	int channel = -1;

	juce::Rectangle<int> area, name, mute, solo, note, wave, keys, meter;
};

/** The rows a column of `width` shows, and how tall they are together. */
struct Layout
{
	std::vector<Row> rows;
	int contentHeight = 0;
};

/** Lays out one row per channel, with a header whenever the chip changes.

	`chipNames` is parallel to the snapshot's channels, `scrollOffset` is how far the content has
	been scrolled up, and `top` is where the scrollable area begins. `contentHeight` is measured
	without the scroll offset, so it does not shrink as the view scrolls.
*/
Layout layoutRows (const std::vector<std::string>& chipNames,
				   int width,
				   int scrollOffset,
				   int top,
				   const RowMetrics& metrics = {});

/** The scroll offset clamped to what the content allows. */
int clampScroll (int scrollOffset, int contentHeight, int viewportHeight);

} // namespace toybox::ui::keyboard
