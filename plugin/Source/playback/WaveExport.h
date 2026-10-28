#pragma once

#include "playback/ChipState.h"

#include <cstddef>
#include <string>
#include <vector>

namespace toybox::playback
{

/** One cycle of a channel's shape, values in -1..1, empty when the channel has no shape.

	The core's own `wave` when the backend supplies one, otherwise the shape its registers describe -
	which is the same synthesis the panel draws with, so the file and the picture cannot disagree.
	They did until this existed: the exporter required a non-empty `wave` and skipped everything
	else, and only FDS and N163 ever supply one. Every channel on every other chip was drawn on
	screen and refused on export.
*/
std::vector<float> cycleOf (const ChannelState& channel);

/** Whether this channel's shape is a period at all.

	A stored table or a synthesised shape is; a sampled channel is not. The panel draws a mark for a
	DPCM or ADPCM row rather than its data - WaveShape synthesises that mark from the sample's
	identity, deliberately, so two samples in one song look different - and a mark is not a
	waveform. Those channels want a PCM export, which does not exist yet, so the caller skips them
	instead of writing a wavetable that describes nothing.
*/
bool hasPeriodicShape (const ChannelState& channel);

/** The rate every exported wavetable is written at. */
constexpr int kWavetableRate = 44100;

/** The length every exported wavetable is resampled to. */
constexpr std::size_t kWavetableSamples = 2048;

/** Writes one cycle of `channel` as a wavetable a modern synth can load.

	A wavetable is a shape, not a recording. Serum and its neighbours take the whole file as one
	period and resample it to whatever length they use internally, so what they need is a single
	cycle at a known rate - not the note that was playing.

	That is the opposite of what this wrote before, and both differences matter. The rate used to be
	`frequency * length`, chosen so that looping the file reproduced the original pitch; a rate
	derived from the note makes the file's pitch a property of the song rather than of the shape.
	And the cycle was repeated sixteen times so a sampler would not reject a 32-sample file - but
	sixteen periods in the file read as a wave that oscillates sixteen times per cycle.

	So: one period, kWavetableSamples samples, kWavetableRate, linear interpolation, wrapping at the
	end so the last sample meets the first.

	This writes shapes. A DPCM or ADPCM channel's sample is a recording - a start, an end, and no
	period - and belongs to a different entry point that does not exist yet. Callers are expected to
	skip those channels; writeCycleWav() writes whatever cycle it is given.
*/
bool writeCycleWav (const ChannelState& channel, const std::string& path);

} // namespace toybox::playback
