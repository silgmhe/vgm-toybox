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

/** How many bits the chip itself produces for this kind of shape, as a count of bits.

	Not a preference and not a guess: the 2A03's pulse is a single bit, its triangle counts to
	thirty-two and is therefore four, the VRC6's saw is a four-bit counter, and an FM voice is an
	analogue sum that has no steps at all. A stored table is six, the widest any backend here
	supplies (FDS); N163's four bits fit inside that without losing anything.
*/
int nativeBits (int waveKind);

/** `value` snapped to `bits` levels across -1..1.

	Sixteen bits is 32768 steps and changes nothing anyone can hear; one bit is a square wave no
	matter what came in. The interesting settings are in between, and they are the point: a wavetable
	synth resamples and band-limits what it is given, and a shape with four or eight steps has
	edges coarse enough to survive that.
*/
float quantiseSample (float value, int bits);

/** The rate a single-period wavetable is written at. */
constexpr int kWavetableRate = 44100;

/** The length a single-period wavetable is resampled to. */
constexpr std::size_t kWavetableSamples = 2048;

/** The pitch a looped export is written at, so every file plays at concert pitch whatever note the
	channel was playing when it was captured.

	440 rather than the channel's own frequency: a set of loops all at A4 can be compared, layered
	and tuned against each other, and a channel whose frequency is unknown - which is most of them
	when the transport is paused - still produces something that sounds like a note.
*/
constexpr double kReferenceHz = 440.0;

/** The quantisation choices the export window offers. */
constexpr int kNativeBits = 0;		// what the chip produces, per channel
constexpr int kMaxBits = 16;

/** How a file is written, because the two destinations want opposite things. */
enum class ExportMode
{
	/** One period: what a wavetable synth reads. The rate is fixed and the shape is resampled to
		kWavetableSamples, because those synths take the whole file as one cycle at any rate. */
	single,

	/** The cycle repeated at kReferenceHz: what a sampler reads. The rate is derived from the
		cycle's own length so that looping reproduces the pitch, and the length is the user's
		choice in seconds. */
	looped
};

/** Writes one cycle of `channel` as a wavetable a modern synth can load.

	The samples are written by repeating each source sample rather than interpolating between them,
	which keeps a staircase a staircase: the chip quantises, the panel draws those steps, and
	smoothing them into a ramp would be drawing a shape the hardware never produced.

	`bits` is the quantisation to apply on the way out, on top of whatever the shape already has.
	kNativeBits means "whatever this chip produces" - which for most shapes is no change at all,
	since the source is already that coarse - and any other value forces every channel to that depth,
	including the ones the chip made finer.
*/
bool writeCycleWav (const ChannelState& channel, const std::string& path,
					ExportMode mode = ExportMode::single, double seconds = 2.0,
					int bits = kNativeBits);

/** `name` as a filename: spaces become hyphens, and the characters a filesystem cannot carry
	become underscores.

	Its own function rather than juce::File::createLegalFileName so that the export can be tested
	without JUCE, and so that the rule is written down in one place: what is illegal on Windows is
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

	Named `<chip>_<channel>.wav`, so that two chips in one song - which both have a "Pulse 1" - do
	not collide. A name that would repeat within one export gets `_2`, `_3` on the end rather than
	losing the file.

	`bits` goes through to writeCycleWav() per channel: kNativeBits asks each shape for its own
	depth, anything else forces one depth on all of them.
*/
ExportOutcome exportWavetables (const ChipStateSnapshot& snapshot, const std::string& folder,
								ExportMode mode = ExportMode::single, double seconds = 2.0,
								int bits = kNativeBits);

} // namespace toybox::playback
