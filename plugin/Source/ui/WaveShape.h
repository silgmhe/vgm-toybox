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
	delta = 5,
	saw = 6,		// the VRC6's ramp: a counter that climbs until it overflows
	fm = 7			// the VRC7: two operators, a carrier and a modulator, no fixed shape
};

/** One period of the shape, values in -1..1, or empty when there is nothing to synthesise.

	`duty` is the pulse width as a fraction of the period and is ignored by the other kinds.
*/
/** One cycle of a channel's shape.

	`shapeParameter` is what the noise and sample kinds need beyond their kind: the noise
	channel's period, or a DPCM sample's identity. Defaults to zero, which is the fixed-shape
	channels' answer.
*/
std::vector<float> cycleFor (int waveKind, float duty, int shapeParameter = 0);

} // namespace toybox::ui::waveform
