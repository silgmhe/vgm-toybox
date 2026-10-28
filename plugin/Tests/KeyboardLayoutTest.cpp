/*
	The keyboard's arithmetic, tested as arithmetic.

	Everything here used to live inside a private nested class in ContentSlot.cpp, which meant the
	only way to check it was to render the panel and look at it. Four of the seven fixes that
	touched that file were arithmetic errors - a height that came out zero, a key count that rounded
	the wrong way, a block that overflowed its card - and none of them needed a screenshot to catch,
	just a function that could be called with a number and asked what it returned.

	Checks: the octave a note is named in, the note of a given white key, how many octaves a column
	is worth, that the span never describes a key past the top of the MIDI range, that it always
	ends on a white key, that a shift is refused exactly when it would lose keys, and - last - which
	columns a new snapshot forces to be repainted.

	Run: <build dir>/plugin/vgm_keyboard_test
*/

#include <cstdio>

#include "ui/KeyboardLayout.h"
#include "ui/KeyboardRowLayout.h"
#include <cmath>

#include "playback/WaveShape.h"
#include "TestSupport.h"

using namespace toybox::ui;

namespace
{

void testNaming (void)
{
	// MIDI 60 is C3 to FL and Yamaha (this project's default), C4 to Ableton and Logic.
	check (octaveShiftForMiddleC (3) == 2, "C3 means a shift of two octaves");
	check (octaveShiftForMiddleC (4) == 1, "C4 means a shift of one octave");
	check (octaveShiftForMiddleC (5) == 0, "C5 means no shift at all");

	// The clamp is what stops a hand-edited settings file from naming notes in another universe.
	check (octaveShiftForMiddleC (99) == 0, "an absurd middle C falls back to C5");
	check (octaveShiftForMiddleC (-3) == 3, "and a negative one to C2");

	check (midiNoteName (60, 2) == "C3", "middle C is C3 with the default shift");
	check (midiNoteName (60, 1) == "C4", "and C4 with the international one");
	check (midiNoteName (61, 2) == "C#3", "sharps keep their sign");
	check (midiNoteName (0, 2) == "C-2", "the bottom of the range is C-2, not C-1");
	check (midiNoteName (127, 2) == "G8", "and the top is G8");
	check (midiNoteName (-1, 2) == "-", "a silent channel has no note name");
}

void testWhiteKeys (void)
{
	// A C major octave: C D E F G A B, then the next C.
	const int base = 36;		// C1 under the default naming
	const int expected[8] = { 36, 38, 40, 41, 43, 45, 47, 48 };

	for (int k = 0; k < 8; ++k)
	{
		check (keyboard::noteForWhite (base, k) == expected[k],
			   "white key " + juce::String (k) + " is the expected semitone");
	}

	check (keyboard::octaveFloor (60) == 60, "a C floors to itself");
	check (keyboard::octaveFloor (71) == 60, "B floors to the C below it");
	check (keyboard::octaveFloor (0) == 0, "C-2 floors to itself");
	check (keyboard::octaveFloor (-1) == -12, "and the note below it floors into the previous octave");
}

void testSpans (void)
{
	// Twenty pixels a key: a 700 pixel column is thirty-five white keys, five octaves.
	{
		const auto span = keyboard::spanFor (700, 36);

		check (span.whiteKeys > 30, "a wide column shows more than four octaves");
		check (span.keyWidth >= 19.0f && span.keyWidth <= 21.0f, "the keys stay about twenty pixels");

		// The block must end on a white key: a span whose last key is a black one would draw a
		// half bar at the right edge.
		const int pc = span.lastNote % 12;
		check (pc == 0 || pc == 2 || pc == 4 || pc == 5 || pc == 7 || pc == 9 || pc == 11,
			   "the last key of a span is a white one");
	}

	// Narrow: the count gives way to a floor of fourteen white keys, and the width shrinks to fit.
	{
		const auto span = keyboard::spanFor (200, 36);

		check (span.whiteKeys >= 14, "a narrow column still shows two octaves");
		check (span.keyWidth < 20.0f, "and pays for it with narrower keys");
	}

	// Near the top of the range the span cannot describe keys that do not exist.
	{
		const auto span = keyboard::spanFor (2000, 120);		// C8 upwards

		check (span.lastNote <= keyboard::lastMusicalNote, "a span never runs past the MIDI range");
		check (span.whiteKeys <= 8, "and there are only a few white keys left up there");
	}

	// A very wide column is filled without inventing notes past G8.
	{
		const auto span = keyboard::spanFor (4000, 0);

		check (span.lastNote <= keyboard::lastMusicalNote, "a huge window still stops at the top");
	}
}

void testShifts (void)
{
	const auto span = keyboard::spanFor (700, 60);

	check (keyboard::canShift (span, -1), "a block in the middle can move down an octave");

	// Right is allowed only while the same number of keys still fits: shifting into the top of the
	// range shortens the block, which is the fault that made the right arrow shrink the keyboard.
	const auto atTop = keyboard::spanFor (700, 96);
	check (! keyboard::canShift (atTop, 1), "a block that would be shortened cannot move right");

	const auto atBottom = keyboard::spanFor (700, 0);
	check (! keyboard::canShift (atBottom, -1), "and the bottom of the range cannot move left");
}

void testWaveShapes (void)
{
	using namespace toybox::playback::waveform;

	// A pulse is high for its duty and low for the rest; the width is clamped so neither half
	// disappears, which would draw a flat line.
	{
		const auto quarter = cycleFor ((int) Kind::pulse, 0.25f);
		check (quarter.size () == 32, "a pulse cycle is thirty-two steps");
		check (quarter[0] > 0.5f && quarter[7] > 0.5f, "the first quarter is high");
		check (quarter[8] < -0.5f && quarter[31] < -0.5f, "and the rest is low");

		const auto empty = cycleFor ((int) Kind::pulse, 0.0f);
		check (empty[0] > 0.5f && empty[1] < -0.5f, "a zero duty still has one high step");

		const auto full = cycleFor ((int) Kind::pulse, 1.0f);
		check (full[31] < -0.5f, "and a hundred percent duty still has one low step");
	}

	// The triangle is a staircase: it has to start at the bottom, climb, and come back.
	{
		const auto tri = cycleFor ((int) Kind::triangle, 0.5f);
		check (tri.size () == 32, "a triangle cycle is thirty-two steps");
		check (tri.front () < -0.99f, "it starts at the bottom");
		check (tri[15] > 0.9f, "reaches the top half way through");
		check (tri.back () < -0.99f, "and returns to the bottom");
	}

	// FM is computed from the patch now, not from one formula with a fixed depth and ratio.
	// Every voice in every song used to draw the same curve, which said nothing about the sound
	// being played - and the parameters were not even reaching this function.
	{
		// Packing round-trips, or a curve would be drawn from the wrong numbers.
		const int packed = packFm (3, 40, 2, 6);
		check (fmMultiple (packed) == 3, "the multiple survives packing");
		check (fmLevel (packed) == 40, "so does the level");
		check (fmWaveform (packed) == 2, "and the waveform");
		check (fmFeedback (packed) == 6, "and the feedback");

		// The level is the modulation index: a loud modulator bends the carrier far harder than
		// a quiet one, so the two curves cannot agree.
		const auto quiet = cycleFor ((int) Kind::fm, 0.5f, packFm (1, 60, 0, 0));
		const auto bright = cycleFor ((int) Kind::fm, 0.5f, packFm (1, 0, 0, 0));

		check (quiet.size () == 32, "an FM cycle is thirty-two steps");

		float widest = 0.0f;

		for (std::size_t i = 0; i < quiet.size () && i < bright.size (); ++i)
		{
			const float distance = std::abs (bright[i] - quiet[i]);

			if (distance > widest)
			{
				widest = distance;
			}
		}

		check (widest > 0.2f, "a deep patch and a shallow one draw different curves");

		// And the shape is a function of the parameters, not of the channel that reported them.
		const auto again = cycleFor ((int) Kind::fm, 0.5f, packFm (1, 0, 0, 0));
		check (again == bright, "the same patch draws the same curve");

		// The modulator runs at the multiple: at half speed the voice needs two carrier periods
		// before it repeats, which is why this differs from the ratio-one case at all.
		const auto halfSpeed = cycleFor ((int) Kind::fm, 0.5f, packFm (0, 24, 0, 0));
		check (halfSpeed != quiet, "half-speed modulation is its own shape");
	}

	// Noise is deterministic: two calls agree, and the values are the two rails.
	{
		const auto a = cycleFor ((int) Kind::noise, 0.5f);
		const auto b = cycleFor ((int) Kind::noise, 0.5f);
		check (a.size () == 32, "a noise cycle is thirty-two steps");
		check (a == b, "the same burst every time, so the display does not shimmer");

		bool railsOnly = true;
		for (float v : a) { railsOnly = railsOnly && (std::abs (v) == 1.0f); }
		check (railsOnly, "every step is at one rail or the other");
	}

	// The period is what makes the sixteen settings sound different, so it has to reach the
	// drawing: period 0 is a fine hiss, period 15 a slow rumble.
	{
		const auto fine = cycleFor ((int) Kind::noise, 0.5f, 0);
		const auto coarse = cycleFor ((int) Kind::noise, 0.5f, 15);

		int fineFlips = 0;
		int coarseFlips = 0;

		for (std::size_t i = 1; i < fine.size (); ++i)
		{
			if (fine[i] != fine[i - 1]) { ++fineFlips; }
			if (coarse[i] != coarse[i - 1]) { ++coarseFlips; }
		}

		// A single set bit as the seed left the register's low bits zero for fourteen shifts, and
		// the old drawing had four flips in thirty-two steps - a flat line with one spike. This is
		// the check that keeps that from coming back.
		check (fineFlips >= 8, "the finest period still reads as a burst, not a flat line");
		check (fineFlips > coarseFlips, "and a coarser period draws a coarser burst");
	}

	// Delta integrates its bits and stays inside the rails.
	{
		const auto d = cycleFor ((int) Kind::delta, 0.5f);
		check (d.size () == 32, "a delta cycle is thirty-two steps");

		bool inside = true;
		bool moved = false;
		for (float v : d)
		{
			inside = inside && v >= -1.0f && v <= 1.0f;
			moved = moved || v != d.front ();
		}
		check (inside, "the integrated level never leaves -1..1");
		check (moved, "and it is not a flat line");
	}

	// Two samples in one song are two marks. A fixed pattern drew a regular staircase, which is
	// indistinguishable from the VRC6 ramp two rows below it - the row said "sample" and looked
	// like "saw".
	{
		const auto kick = cycleFor ((int) Kind::delta, 0.5f, 0xC0000);
		const auto hat = cycleFor ((int) Kind::delta, 0.5f, 0xC0400);

		check (kick != hat, "two samples do not draw the same shape");

		// Regular means periodic: a walk that repeats with a short period reads as a waveform, not
		// as a sample. The old `i % 7` pattern repeated five times in one cycle.
		int repeats = 0;

		for (std::size_t start = 1; start <= 8; ++start)
		{
			bool periodic = true;

			for (std::size_t i = start; i < kick.size (); ++i)
			{
				if (std::abs (kick[i] - kick[i - start]) > 0.001f)
				{
					periodic = false;
					break;
				}
			}

			if (periodic) { ++repeats; }
		}

		check (repeats == 0, "and neither of them repeats on a short cycle");
	}

	// Nothing to synthesise: the stored-table kind and anything unknown leave it to the backend.
	check (cycleFor ((int) Kind::none, 0.5f).empty (), "kind 0 has no shape");
	check (cycleFor ((int) Kind::stored, 0.5f).empty (), "a stored table is not synthesised");
	check (cycleFor (99, 0.5f).empty (), "and an unknown kind draws nothing");
}

void testRowLayout (void)
{
	using namespace toybox::ui::keyboard;

	// One chip, three channels: a header and three rows, in that order.
	{
		const auto layout = layoutRows ({ "2A03", "2A03", "2A03" }, 900, 0, 44);

		check (layout.rows.size () == 4, "three channels on one chip make four rows");
		check (layout.rows[0].header, "the first row is the chip's title");
		check (layout.rows[0].title == "2A03", "and it is named after the chip");
		check (! layout.rows[1].header, "the rows after it are channels");
		check (layout.rows[1].channel == 0 && layout.rows[3].channel == 2, "in snapshot order");
	}

	// Two chips: a second header exactly where the chip changes.
	{
		const auto layout = layoutRows ({ "2A03", "2A03", "N163" }, 900, 0, 44);

		check (layout.rows.size () == 5, "two chips make five rows");
		check (layout.rows[3].header && layout.rows[3].title == "N163",
			   "the header sits before the first channel of the new chip");
	}

	// Rows stack downward and never overlap.
	{
		const auto layout = layoutRows ({ "2A03", "2A03", "2A03" }, 900, 0, 44);

		bool stacked = true;
		for (std::size_t i = 1; i < layout.rows.size (); ++i)
		{
			stacked = stacked && layout.rows[i].area.getY () >= layout.rows[i - 1].area.getBottom ();
		}

		check (stacked, "each row starts at or below the end of the one above");
	}

	// The columns sit inside the card, in order, and do not overlap.
	{
		const auto layout = layoutRows ({ "2A03" }, 900, 0, 44);
		const auto& row = layout.rows[1];

		check (row.name.getX () >= row.area.getX (), "the name starts inside the card");
		check (row.meter.getRight () <= row.area.getRight (), "and the meter ends inside it");
		check (row.name.getRight () <= row.mute.getX (), "name then mute");
		check (row.mute.getRight () <= row.solo.getX (), "mute then solo");
		check (row.solo.getRight () <= row.note.getX (), "solo then the note column");
		check (row.note.getRight () <= row.wave.getX (), "the note column then the waveform");
		check (row.wave.getRight () <= row.keys.getX (), "the waveform then the keyboard");
		check (row.keys.getRight () <= row.meter.getX (), "and the keyboard before the meter");
		check (row.mute.getHeight () == 30 && row.mute.getWidth () == 30,
			   "the mute button is the mixer's size");
	}

	// The keyboard is centred vertically in its row and does not fill it.
	{
		const auto layout = layoutRows ({ "2A03" }, 900, 0, 44);
		const auto& row = layout.rows[1];

		check (row.keys.getHeight () == 32, "the keyboard is thirty-two tall in a forty-eight row");
		check (row.keys.getY () > row.area.getY () && row.keys.getBottom () < row.area.getBottom (),
			   "with room above and below");
	}

	// Content height is measured without the scroll, so it does not shrink as the view scrolls.
	{
		const auto top = layoutRows ({ "2A03", "2A03", "N163" }, 900, 0, 44);
		const auto scrolled = layoutRows ({ "2A03", "2A03", "N163" }, 900, 120, 44);

		check (top.contentHeight == scrolled.contentHeight,
			   "scrolling does not change how tall the content is");
		check (scrolled.rows[1].area.getY () == top.rows[1].area.getY () - 120,
			   "it only moves the rows up");
	}

	// Narrow windows keep a usable card width rather than a negative one.
	{
		const auto layout = layoutRows ({ "2A03" }, 60, 0, 44);
		check (layout.rows[1].area.getWidth () >= 200, "a narrow window keeps a minimum card width");
	}
}

void testScrollClamping (void)
{
	using namespace toybox::ui::keyboard;

	check (clampScroll (0, 100, 400) == 0, "content shorter than the view does not scroll");
	check (clampScroll (50, 100, 400) == 0, "and an offset inside it is pulled back to zero");
	check (clampScroll (0, 1000, 400) == 0, "a long list starts at the top");
	check (clampScroll (600, 1000, 400) == 600, "the last row can be reached");
	check (clampScroll (9999, 1000, 400) == 600, "and not scrolled past");
	check (clampScroll (-40, 1000, 400) == 0, "a negative offset is zero");
}

} // namespace

/* Which columns a new snapshot forces to be redrawn.

	This is the decision that decides how much of the GPU's work happens, and its failure mode is
	the quiet one: a column that stops updating never throws, never fails a build, and looks fine
	until somebody notices a note frozen at its old value. Inside the panel it could only be
	observed by playing a song; out here it is nine assertions.
*/
void testRowChangeDecision (void)
{
	// Two identical signatures: nothing has moved, so nothing is drawn. This is the state most
	// frames of a held note are in, and it is what makes the panel cost nothing at rest.
	{
		keyboard::RowSignature steady;
		steady.note = 60;
		steady.volumeStep = 128;
		steady.waveKind = 2;
		steady.dutyStep = 32;
		steady.shapeParameter = 1;
		steady.waveSize = 16;

		check (! keyboard::compareRow (steady, steady, false).any (),
			   "an unchanged row needs no repaint at all");
	}

	// A default signature has note == -2, which no real note is: that is the never-painted state.
	{
		const keyboard::RowSignature never;
		keyboard::RowSignature first;
		first.note = 60;

		check (keyboard::compareRow (never, first, false).wholeRow,
			   "a row that has never been painted is repainted whole");
	}

	// A note moving repaints the note cell and the keys, and nothing else - not the meter, not the
	// waveform, not the card behind them.
	{
		keyboard::RowSignature before;
		before.note = 60;
		before.volumeStep = 128;

		auto after = before;
		after.note = 64;

		const auto change = keyboard::compareRow (before, after, false);
		check (change.note, "a new note repaints the note column");
		check (! change.wave && ! change.meter && ! change.wholeRow,
			   "and leaves the waveform, the meter and the card alone");
	}

	// The level moving repaints the meter only.
	{
		keyboard::RowSignature before;
		before.note = 60;
		before.volumeStep = 128;

		auto after = before;
		after.volumeStep = 200;

		const auto change = keyboard::compareRow (before, after, false);
		check (change.meter && ! change.note && ! change.wave,
			   "a new level repaints the meter column");
	}

	// Crossing silence is not a meter change: the name column is drawn in the primary colour when
	// the row is audible and the secondary one when it is not, so the whole row has to be redrawn.
	{
		keyboard::RowSignature before;
		before.note = 60;
		before.volumeStep = 0;

		auto after = before;
		after.volumeStep = 3;

		check (keyboard::compareRow (before, after, false).wholeRow,
			   "crossing the silence threshold repaints the whole row, not just the meter");
	}

	// Mute and solo are whole-row facts: the button, its glow and the name's colour all change.
	{
		keyboard::RowSignature before;
		before.note = 60;
		before.volumeStep = 128;

		auto after = before;
		after.muted = true;

		check (keyboard::compareRow (before, after, false).wholeRow,
			   "toggling mute repaints the whole row");
	}

	// A register moving repaints the waveform.
	{
		keyboard::RowSignature before;
		before.note = 60;
		before.waveKind = 2;

		auto after = before;
		after.waveKind = 3;

		check (keyboard::compareRow (before, after, false).wave,
			   "a new wave shape repaints the waveform column");
	}

	// A channel carrying the core's own cycle is never skipped: its contents can change while its
	// size stays the same, so no comparison of the signature can rule it out.
	{
		keyboard::RowSignature steady;
		steady.note = 60;

		check (keyboard::compareRow (steady, steady, true).wave,
			   "a channel with its own cycle repaints its waveform every time");
	}

	// The quantisation is shared with WavePathCache, so "one step" means the same thing to both.
	check (keyboard::dutySteps (0.0f) == 0, "duty 0 is step 0");
	check (keyboard::dutySteps (0.5f) == 32, "and half is 32");
	check (keyboard::dutySteps (1.0f) == 64, "duty 1 is step 64");
	check (keyboard::volumeSteps (0.0f) == 0 && keyboard::volumeSteps (1.0f) == 255,
		   "volume spans 0 to 255");
}

int main (void)
{
	std::printf ("keyboard layout and wave shapes\n");

	testNaming ();
	testWhiteKeys ();
	testSpans ();
	testShifts ();
	testWaveShapes ();
	testRowLayout ();
	testScrollClamping ();
	testRowChangeDecision ();

	std::printf ("%s\n", toybox::test::failures() == 0 ? "all checks passed" : "FAILURES");

	return toybox::test::failures() == 0 ? 0 : 3;
}
