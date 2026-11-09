#pragma once

#include "playback/ChipState.h"

#include <cstddef>
#include <ctime>
#include <functional>
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
	else is written as one period. The pair a backend fills together - the rate says the shape is
	bytes and the wave says there are bytes - is necessary and **not sufficient**: a wavetable also
	declares a rate, because it walks its table per second, so `sampleRate > 0` alone cannot tell
	the two apart.

	**That is what `tableWave` is for**, and it is the one mistake this function has made: with only
	the rate and the wave to go on, a VSU table was exported as a recording - raw floats at its
	entry rate, no quantisation, no one-period marker - and a synth then read it as audio and
	interpolated a shape that should have stayed stepped. The user's report was "the bit depth
	setting does nothing", which is exactly what the wrong path looks like from outside.

	One definition, because there were two: the exporter decided with it and the sample writer
	checked it again in the negative, so a change to either would have let a file pass the first
	gate and fail the second, silently.
*/
bool isSampledChannel (const ChannelState& channel);

/** Whether this channel would produce a file at all.

	Separate from writeChannelShape because the answer is needed before a name is chosen: the batch
	export numbers repeated names in the order it meets them, and a channel that writes nothing must
	not consume a number. It did once - a skipped channel took `_2` and the real file behind it got
	`_3`, with both still written under names that looked right.
*/
bool hasSomethingToWrite (const ChannelState& channel);

/** What happened when one channel was written out. */
enum class ShapeWrite
{
	written,		// the file is on disk

	/** The channel has nothing to write: not a sample, and not a period either - MMC5's PCM before
		anything was captured, a channel the backend never filled. Not an error, and the difference
		between this and `failed` is the whole diagnosis when a count comes back short. */
	nothingToWrite,

	failed			// it had something and the write did not land
};

/** Writes one channel as whatever it is: a sample as audio, a period as a wavetable.

	The three answers are the reason this exists next to writeCycleWav rather than on top of it:
	the batch export has to tell "nothing to write" from "tried and failed" to report both, and a
	drag has to refuse to start on the first while offering to explain the second. Deciding that in
	two places is how the two exports drift - and this branch has been wrong before, in the
	direction of skipping every channel except FDS and N163.

	`bits` only concerns periods. A sample's depth is its own encoding; quantising it is a different
	operation and not one anybody asked for.
*/
ShapeWrite writeChannelShape (const ChannelState& channel, const std::string& path,
							  ExportMode mode = ExportMode::single, double seconds = 2.0,
							  int bits = kNativeBits);

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

/** A filename for a wavetable dragged out of the window: `<stem>_<YYYY_MMDD_HHMMSS>.wav`.

	The timestamp is to the second, because two drags inside one second is a thing a hand can do
	and a name has to survive it: a repeat takes `-2`, then `-3`. It is also the only record of when
	the file was made that survives a copy, which the cache cleaner ends up caring about.

	`local` is passed in rather than read from the clock, and `exists` rather than the filesystem,
	because that is the whole of what this function needs from the outside - and both of those are
	what make it a function a test can call.

	`stem` comes from the channel (`<chip>_<channel>`), so `2A03_Pulse 1` and `N163_Wave 8` do not
	collide when both are dragged into the same synthesiser.
*/
std::string dragExportName (const std::string& stem, const std::tm& local,
							const std::function<bool (const std::string&)>& exists);

/** Where dragged wavetables are written: `<temp>/VGMToybox/drag-export`.

	A folder of our own rather than the temp root, so the cleaner can be narrow: it deletes only
	what is in here, and "is this one of ours" needs no parsing of a name.

	Not created here. The first drag creates it, and a start-up that finds no folder cleans nothing
	- which is the right answer for a fresh install.
*/
std::string dragExportFolder (void);

/** Deletes the `.wav` files in `folder` that have not been touched for `retentionSeconds`.

	Returns how many went. A folder that does not exist is not an error and removes nothing: the
	cache directory is created by the first drag, so the first start-up after an install finds none.

	Only `.wav`, only this level, and no recursion. The folder belongs to us, but a path computed
	wrongly should not turn into "emptied the temp directory" - which is the sort of thing a
	cleaner is one bug away from, and the reason this one is deliberately narrow.

	Age comes from the filesystem's own clock rather than the wall clock: a machine whose date was
	changed does not make the whole cache look expired, and it is the one comparison a test can
	arrange without waiting an hour.
*/
int clearDragCache (const std::string& folder, double retentionSeconds);

/** One decoded sample, ready to be written: its levels, the rate they play at, and its name.

	Levels rather than bytes, because the encoding is the backend's business - the DPCM decoder
	lives with the NES backend, and a PCM chip's would live with its own. What is shared is the
	container, the naming, and the place the files go.
*/
struct DecodedSample
{
	std::string name;			// without the extension
	std::vector<float> values;
	double rate = 0.0;

	/** Which sample this is, in the same terms the chip's registers use.

		Here so a caller that watches a channel play - rather than a file being exported - can tell
		what it is hearing. The alternative was to rebuild the bytes from the chip's memory at
		display time, which is a second reading of the same data and can disagree with this one.

		Zero means the producer did not name it, which is every non-DPCM sample bank today.
	*/
	uint32_t sampleId = 0;
};

/** Writes one sample as a float32 mono WAV at `rate`, with no `clm ` marker.

	The container writeSampleWav() produces, for a caller that already has levels rather than a
	ChannelState to decode.
*/
bool writeWavFile (const std::string& path, const std::vector<float>& values, double rate);

/** Writes every sample into `folder`, named `<name>.wav`, returning how many were written.

	One file failing to open is counted as not written and the rest still go: a bad name should not
	cost the export its other four. Names pass through legalFileName(), so an address like `$C0C0`
	becomes one hyphen-separated token rather than a path.
*/
/** `maxSeconds` truncates each file to that length; 0 writes the whole sample.

	The limit is applied **here** rather than in each collector: every chip's samples arrive as
	levels and a rate, so one rule covers all of them (and the next chip) without any of them
	knowing about the setting. `truncated`, when given, counts the files that were cut.
*/
int writeSamples (const std::vector<DecodedSample>& samples, const std::string& folder,
				  double maxSeconds = 0.0, int* truncated = nullptr);

} // namespace toybox::playback
