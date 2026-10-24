#pragma once

#include <vector>

namespace toybox::ui::waveform
{

/** How a channel makes its shape, as the backend describes it (see playback/ChipState.h).

	The panel draws whatever arrives; this function is the fallback for the channels that generate
	a shape rather than read one from memory. Keeping it here means the drawing code has no case
	statement per chip, and that the shapes can be checked without rendering anything.
*/
enum class Kind
{
	none = 0,		// nothing to draw
	stored = 1,		// the backend supplied `ChannelState::wave`
	pulse = 2,
	triangle = 3,
	noise = 4,
	delta = 5
};

/** One period of the shape, values in -1..1, or empty when there is nothing to synthesise.

	`duty` is the pulse width as a fraction of the period and is ignored by the other kinds.
*/
std::vector<float> cycleFor (int waveKind, float duty);

} // namespace toybox::ui::waveform
