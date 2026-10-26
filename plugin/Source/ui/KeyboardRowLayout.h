#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <cstddef>
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

	/** Width kept clear at the card's right edge for its scroll bar. Every row gives it up,
		headers included: a header that ran under the bar had its "5 ch" label covered by the thumb. */
	int reservedRight = 0;

	int rowInsetLeft = 14;		// where the columns start inside the card
	int rowInsetRight = 14;
	int nameWidth = 72;
	int buttonSize = 30;		// the mixer's button height, so the two views agree
	int buttonGap = 8;
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

	/** How many channels the header above them stands for. Zero on a channel row.

		Counted while the rows are built rather than while they are drawn: the panel used to run a
		count_if over every channel for every visible header, once per frame, constructing a
		juce::String per comparison - for a number that cannot change until the song does.
	*/
	int chipCount = 0;

	juce::Rectangle<int> area, name, mute, solo, note, wave, keys, meter;
};

/** What one row looked like the last time it was painted: everything paintChannelRow() reads, and
	nothing else.

	The quantisation is deliberate. A float volume compared exactly would differ in the last bit
	most frames and repaint the row anyway, which is the cost this exists to avoid; the meter is
	96 px wide, so an eighth of a percent is beneath what anyone can see.

	It lives here rather than inside the panel because the decision it feeds is arithmetic - two
	values in, a set of rectangles out. Inside the panel the only way to check it was to play a
	song and watch for a column that had stopped moving, which is the failure mode rather than a
	test.
*/
struct RowSignature
{
	// The row as a whole. These change only when the user does something - a click on M or S, or
	// the song starting and stopping - so they cost a whole-row repaint, shadow included.
	bool active = false;
	bool analysing = false;
	bool muted = false;
	bool soloed = false;

	// The columns that move while a song plays. Compared apart from the above because each one
	// owns a rectangle of its own: a note moving repaints the note cell and the keys, not the
	// row's name, its buttons, its waveform, its meter and the card behind all of them.
	int note = -2;
	int volumeStep = -1;
	int waveKind = -1;
	int dutyStep = -1;
	int shapeParameter = -1;
	std::size_t waveSize = 0;
};

/** Which of a row's columns have to be repainted. */
struct RowChange
{
	bool wholeRow = false;
	bool note = false;
	bool wave = false;
	bool meter = false;

	bool any (void) const { return wholeRow || note || wave || meter; }
};

/** Duty and volume as the steps the repaint decision compares.

	One source for both halves: the row signature and the waveform cache both ask "has this
	changed", and they have to agree on what changed means. They did not - one compared 1/64 steps
	and the other compared floats exactly - which left the finer comparison unreachable and the
	coarser one unexplained.
*/
inline int dutySteps (float duty)
{
	return (int) std::lround (juce::jlimit (0.0f, 1.0f, duty) * 64.0f);
}

inline int volumeSteps (float volume)
{
	return (int) std::lround (juce::jlimit (0.0f, 1.0f, volume) * 255.0f);
}

/** What must be repainted to get from `before` to `after`.

	`carriesOwnCycle` is the channel's `wave` being non-empty: those are drawn straight from the
	core's snapshot, whose contents can change while its size stays the same, so their waveform
	column is never skipped.

	A row that has never been painted has note == -2 and lands on the whole-row branch. The last
	clause of that branch is not about the note: the name column is drawn in the primary colour
	when the row is audible and the secondary one when it is not, and audible is active && volume
	above silence - so crossing that threshold changes a column this row would not otherwise
	repaint.
*/
RowChange compareRow (const RowSignature& before, const RowSignature& after, bool carriesOwnCycle);

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
