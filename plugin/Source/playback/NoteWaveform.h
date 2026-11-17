#pragma once

#include <cmath>
#include <vector>

#include "chips/VoicePatch.h"

namespace toybox::playback
{

class IVoiceDriver;

namespace noteWave
{

/** **One note, from key-on to silence**, as mono samples in -1..1.

	What the keyboard's little window wants and what a dragged file contains: an FM channel used to
	show one cycle on loop, because one cycle was all the channel state carried.

	**The samples come from a chip, not from a model of one.** `IVoiceDriver` is handed in, so this
	function contains no knowledge of which engine produces them - the libvgm backend passes the core
	the plugin plays through, the NSF backend passes the OPLL its own player uses. An earlier version
	synthesised the envelope from one exponential curve; it had the right shape and the wrong
	instrument, and no constant could close that gap.

	`seconds` of 0 asks `durationOf()` for a length. The note is held for `sustainSeconds` and then
	keyed off, and the rest of the buffer is the chip's release.
*/
std::vector<float> render (IVoiceDriver& driver, const chips::VoicePatch& patch, double frequencyHz,
						   double sustainSeconds = 0.0, double releaseSeconds = 0.0,
						   bool normalise = false);

/** The frequency of a MIDI note, equal temperament with A4 at 440. A convenience for callers that
	think in notes - `render` takes Hz, because that is what a chip reports. */
inline double frequencyOf (int midiNote)
{
	return 440.0 * std::pow (2.0, ((double) midiNote - 69.0) / 12.0);
}

/** The length a caller gets when it asks for none: the two fallbacks added together. Not a
	prediction of how long this patch sounds - see `render` for why that cannot be known. */
double durationOf (const chips::VoicePatch& patch);

} // namespace noteWave
} // namespace toybox::playback
