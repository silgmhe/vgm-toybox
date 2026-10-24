#pragma once

namespace toybox::settings::keyboard
{

/** How long a key stays lit after its note stops.

	A chip's period register holds its last value, and a fast passage has gaps between notes far
	shorter than a display frame. Holding the key for a moment keeps the row from blinking without
	pretending the note is still sounding - the meter empties on time either way.
*/
constexpr int minNoteHoldMs = 0;
constexpr int maxNoteHoldMs = 500;
constexpr int noteHoldStepMs = 10;
constexpr int defaultNoteHoldMs = 120;

} // namespace toybox::settings::keyboard
