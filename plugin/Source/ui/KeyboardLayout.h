#pragma once

#include <juce_core/juce_core.h>

namespace toybox::ui
{

// --- naming ---------------------------------------------------------------------------------
// MIDI 60 is called C3, C4 or C5 depending on who you ask, and the settings page has the answer.
// These two functions are the only place that decides, so a lit key, the note column, an octave
// label and the settings page's own combo cannot disagree.

/** 5 minus the octave number the user gave middle C: C4 gives 1 (the international convention,
	C3 gives 2 (FL Studio and Yamaha, which is where this project starts). */
int octaveShiftForMiddleC (int middleCOctave);

/** "C#3", "G8", "C-2" - or "-" for a channel that is not sounding. */
juce::String midiNoteName (int note, int octaveShift);

// --- the keyboard's arithmetic ---------------------------------------------------------------
namespace keyboard
{

/** The whole MIDI range. What those notes are called is the section above; the range is fixed. */
constexpr int firstMusicalNote = 0;
constexpr int lastMusicalNote = 127;

/** Nominal width of a white key. The real width is derived from how many fit the column. */
constexpr int whiteKeyWidth = 20;

/** Rounds down to the C at or below `note`; works below zero, which C-2 needs. */
int octaveFloor (int note);

/** The note of the k-th white key counted from `base`, which is itself a C. */
int noteForWhite (int base, int k);

/** What one row's keyboard covers, worked out from the room it has. */
struct KeySpan
{
	int base = 36;						// lowest note drawn
	int whiteKeys = 28;
	float keyWidth = (float) whiteKeyWidth;
	int lastNote = 83;
};

/** How many octaves a column of `widthPixels` is worth, before the musical range clamps it. */
int octavesFor (int widthPixels);

/** The span a column of that width shows when it starts at `base`.

	The count of keys is rounded to the nearest, and the width derived from it, so the keyboard
	fills the column at every window width instead of changing a whole key at a time. Nothing
	above `lastMusicalNote` is ever drawn: the key count is clamped to the notes that exist.
*/
KeySpan spanFor (int widthPixels, int base);

/** Whether the window can move by an octave without losing keys off the far end.

	A shift that runs the block into the top of the range makes it shorter, which is why the right
	arrow used to move the window and shrink the keyboard at the same time.
*/
bool canShift (const KeySpan& span, int direction);

} // namespace keyboard

} // namespace toybox::ui
