#pragma once

#include <cmath>
#include <vector>

#include "chips/VoicePatch.h"

namespace toybox::chips::noteWave
{

/** **One note, from key-on to silence**, as samples in -1..1.

	What a panel drawing a whole note wants, and what the keyboard's little window has never had:
	an FM channel used to show one cycle on loop, because one cycle was all the channel state
	carried. A recording already draws its whole length there; this is the same shape for a voice
	that has to be played rather than read.

	**It is not a synthesised approximation: it drives the chip core the plugin plays through.**
	`SndEmu_Start` picks the default YM2612 implementation - the same one `VGMPlayer` uses - so the
	samples here are the samples the listener hears for that patch. An earlier version computed the
	envelope from one exponential curve and the result was visibly the right shape and audibly the
	wrong instrument; that gap cannot be closed by tuning constants, only by asking the chip.

	`seconds` sizes the buffer. The note is held for `kHoldSeconds` and then keyed off; rendering
	stops as soon as it has been quiet for twenty milliseconds, so a blip comes out short and a pad
	long without anything predicting either.
*/
/** The rate this renderer samples at; the constant itself is in `chips/VoicePatch.h`, where the
	exporter can reach it without including an engine header. */
constexpr double kRenderRate = chips::kNoteRate;

/** **A note in two stages**: `sustainSeconds` held, then `releaseSeconds` for the tail.

	Both are preferences rather than properties of the patch - see `kDefaultSustainSeconds` for why
	neither can be read from the registers. A value of 0 in either asks for the default.
*/
std::vector<float> render (const chips::VoicePatch& patch, double frequencyHz,
						   double sustainSeconds = 0.0, double releaseSeconds = 0.0,
						   double rate = 32000.0);

/** The frequency of a MIDI note, equal temperament with A4 at 440. A convenience for callers
	that think in notes - the generator itself takes Hz, because that is what a chip reports. */
inline double frequencyOf (int midiNote)
{
	return 440.0 * std::pow (2.0, ((double) midiNote - 69.0) / 12.0);
}

/** The upper bound on a note's length, in seconds: a buffer size, not a prediction. `render()`
	stops on silence and usually returns less. */
double durationOf (const chips::VoicePatch& patch);

} // namespace toybox::chips::noteWave
