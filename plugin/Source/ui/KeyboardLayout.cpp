#include "KeyboardLayout.h"

namespace toybox::ui
{

int octaveShiftForMiddleC (int middleCOctave)
{
	return 5 - juce::jlimit (2, 5, middleCOctave);
}

juce::String midiNoteName (int note, int octaveShift)
{
	static const char* const names[] = { "C", "C#", "D", "D#", "E", "F",
										 "F#", "G", "G#", "A", "A#", "B" };

	if (note < 0)
	{
		return "-";
	}

	return juce::String (names[note % 12]) + juce::String (note / 12 - octaveShift);
}

namespace keyboard
{

int octaveFloor (int note)
{
	return note - ((note % 12) + 12) % 12;
}

int noteForWhite (int base, int k)
{
	static const int offsets[7] = { 0, 2, 4, 5, 7, 9, 11 };
	return base + (k / 7) * 12 + offsets[k % 7];
}

namespace
{
	/** Two octaves, however narrow the window gets. */
	constexpr int minWhiteKeys = 14;

	/** Below this a key is a smudge rather than a key; the count gives way instead. */
	constexpr float minKeyWidth = 10.0f;

	int whiteKeysFor (int widthPixels)
	{
		return juce::jmax (minWhiteKeys, juce::roundToInt ((float) widthPixels / (float) whiteKeyWidth));
	}
}

int octavesFor (int widthPixels)
{
	return juce::jmax (2, whiteKeysFor (widthPixels) / 7);
}

KeySpan spanFor (int widthPixels, int base)
{
	KeySpan span;

	const int wanted = whiteKeysFor (widthPixels);

	span.keyWidth = juce::jmax (minKeyWidth, (float) widthPixels / (float) wanted);
	span.whiteKeys = wanted;
	span.base = base;

	// Nothing above the top of the range can light, so the keys stop there; the block is then
	// centred by the caller, because a keyboard that ended early and hugged the left edge looked
	// like it had been cut.
	//
	// The octave formula overcounts the last, partial octave - it always adds seven - so the count
	// is then trimmed until the last key is a note that exists. Without this a span could describe
	// C9, which the panel silently skipped at draw time: the invariant belongs here, where it can
	// be asserted, rather than in a `continue` inside a paint loop.
	const int whiteByRange = ((lastMusicalNote - span.base) / 12) * 7 + 7;
	span.whiteKeys = juce::jmin (span.whiteKeys, whiteByRange);

	while (span.whiteKeys > 1 && noteForWhite (span.base, span.whiteKeys - 1) > lastMusicalNote)
	{
		--span.whiteKeys;
	}

	span.lastNote = noteForWhite (span.base, span.whiteKeys - 1);

	return span;
}

bool canShift (const KeySpan& span, int direction)
{
	if (direction > 0)
	{
		const int candidate = span.base + 12;

		if (candidate > lastMusicalNote)
		{
			return false;
		}

		const int whiteByRange = ((lastMusicalNote - candidate) / 12) * 7 + 7;
		return whiteByRange >= span.whiteKeys;
	}

	return span.base - 12 >= firstMusicalNote;
}

} // namespace keyboard

} // namespace toybox::ui
