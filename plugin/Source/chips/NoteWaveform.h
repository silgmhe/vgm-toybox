#pragma once

#include <cmath>
#include <vector>

#include "VoicePatch.h"

namespace toybox::chips::noteWave
{

/** **One note, from key-on to silence**, as samples in -1..1.

	What a panel drawing a whole note wants, and what the keyboard's little window has never had:
	an FM channel used to show one cycle on loop, because one cycle was all the channel state
	carried. A recording already draws its whole length there; this is the same shape for a voice
	that has to be computed.

	`seconds` of 0 asks `durationOf()` for a length - the envelope's own, so a 40 ms blip and a
	two-second pad each fill the window instead of one being squashed and the other clipped.

	**This is a structured approximation, not a sample-exact emulation.** The operators, the eight
	algorithms, the ratios and the total levels are the chip's; the envelope rates are mapped
	through one exponential curve (`secondsPerSweep`) rather than the chip's step table and
	key-scaling, and detune is ignored because a few cents do not survive the pixel. It is for
	drawing. When a voice rig has to make a sound, it uses the real emulator.
*/
std::vector<float> render (const chips::VoicePatch& patch, double frequencyHz,
						   double seconds = 0.0, double rate = 32000.0);

/** The frequency of a MIDI note, equal temperament with A4 at 440. A convenience for callers
	that think in notes - the generator itself takes Hz, because that is what a chip reports. */
inline double frequencyOf (int midiNote)
{
	return 440.0 * std::pow (2.0, ((double) midiNote - 69.0) / 12.0);
}

/** How long the envelope takes for this patch, in seconds. */
double durationOf (const chips::VoicePatch& patch);

} // namespace toybox::chips::noteWave
