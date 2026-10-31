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

	A DPCM channel supplies one too now, and that is why this is not the function to ask whether a
	channel is a wavetable - see hasPeriodicShape().
*/
std::vector<float> cycleOf (const ChannelState& channel);

/** Whether this channel's shape is a period at all.

	A stored table or a synthesised shape is; a sampled channel is not. Those go through
	writeSampleWav() instead, with no marker and at their own rate.

	The panel still draws a mark for a sampled row when no data was captured - WaveShape synthesises
	it from the sample's identity so two samples look different. That mark is not a waveform, which
	is the other half of why this distinction exists: MMC5's PCM has no bank to read and never gets
	real data, so it keeps the mark and keeps being skipped here.
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
/** The widest depth the window offers.

	Thirty-two is what a float sample can hold and what the file already is, so it is the last
	setting that still means something: past that the quantiser has nothing left to snap.
*/
constexpr int kMaxBits = 32;

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

/** Whether this channel carries a sample rather than a shape.

	A sampled channel is written as audio at the rate its bytes are read, in both modes; everything
	else is written as one period. The two conditions are the pair a backend fills together - the
	rate says the shape is bytes and the wave says there are bytes - and both are needed: a channel
	whose sample has not been captured yet has neither.

	One definition, because there were two: the exporter decided with it and the sample writer
	checked it again in the negative, so a change to either would have let a file pass the first
	gate and fail the second, silently.
*/
bool isSampledChannel (const ChannelState& channel);

/** Writes `channel`'s sample as ordinary audio, which is what it is.

	A DPCM hit is a moment, not a period. Writing it with the one-cycle marker would tell a
	wavetable synth that 2048 points of it repeat, and it would loop the hit at a pitch nothing
	plays it at. So there is no marker, no resampling and no quantisation: the bytes the chip reads,
	at the rate it reads them, in the container every audio import understands.

	Fails when the channel carries no sample - a synthesised shape has nothing to write here.
*/
bool writeSampleWav (const ChannelState& channel, const std::string& path);

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
