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
	else, and only FDS and N163 ever supply one.
*/
std::vector<float> cycleOf (const ChannelState& channel);

/** Whether this channel's shape is a period at all.

	A stored table or a synthesised shape is; a sampled channel is not. The panel draws a mark for a
	DPCM or ADPCM row rather than its data - WaveShape synthesises that mark from the sample's
	identity, deliberately, so two samples in one song look different - and a mark is not a
	waveform. Those channels want a PCM export, which does not exist yet.
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
*/
bool writeCycleWav (const ChannelState& channel, const std::string& path);

/** `name` with the characters a filename cannot carry replaced by underscores.

	Its own function rather than juce::File::createLegalFileName so that the export can be tested
	without JUCE - and so that the rule is written down in one place: what is illegal on Windows is
	illegal here, because a wav that cannot be written there is not a wav.
*/
std::string legalFileName (const std::string& name);

/** What an export did.

	`channelsSeen` is the one that matters when nothing was written, and it exists because "0
	written" was ambiguous twice: a snapshot with no channels and a snapshot whose every channel
	failed both report zero, and the difference is the whole diagnosis.
*/
struct ExportOutcome
{
	int channelsSeen = 0;
	int written = 0;
	int skippedSample = 0;
	int failed = 0;
};

/** Writes every periodic channel in `snapshot` into `folder`, which must already exist.

	Named `<chip>_<channel>_<index>.wav`, the index one-based and in snapshot order, so that two
	chips in one song - which both have a "Pulse 1" - do not collide and a file can be traced back
	to the row it came from.
*/
ExportOutcome exportWavetables (const ChipStateSnapshot& snapshot, const std::string& folder);

} // namespace toybox::playback
