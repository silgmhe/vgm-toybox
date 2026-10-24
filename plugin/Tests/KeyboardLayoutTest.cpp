/*
	The keyboard's arithmetic, tested as arithmetic.

	Everything here used to live inside a private nested class in ContentSlot.cpp, which meant the
	only way to check it was to render the panel and look at it. Four of the seven fixes that
	touched that file were arithmetic errors - a height that came out zero, a key count that rounded
	the wrong way, a block that overflowed its card - and none of them needed a screenshot to catch,
	just a function that could be called with a number and asked what it returned.

	Checks: the octave a note is named in, the note of a given white key, how many octaves a column
	is worth, that the span never describes a key past the top of the MIDI range, that it always
	ends on a white key, and that a shift is refused exactly when it would lose keys.

	Run: <build dir>/plugin/vgm_ui_test
*/

#include <cstdio>

#include "ui/KeyboardLayout.h"
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

} // namespace

int main (void)
{
	std::printf ("keyboard layout\n");

	testNaming ();
	testWhiteKeys ();
	testSpans ();
	testShifts ();

	std::printf ("%s\n", toybox::test::failures() == 0 ? "all checks passed" : "FAILURES");

	return toybox::test::failures() == 0 ? 0 : 3;
}
