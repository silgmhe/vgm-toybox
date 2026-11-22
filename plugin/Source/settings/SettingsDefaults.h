#pragma once

namespace toybox::settings
{

/** **Every default the user can change, in one file.**

	The store reads them, the settings pages draw their bounds from them, and the parts that have an
	opinion of their own before a song is opened (the MIDI encoder's clamp, the NSF backend's fallback
	length) read the same numbers. They used to be spread across `KeyboardSettings.h`, `NsfSettings.h`,
	`ExportSettings.h`, three statics on `SettingsStore`, one constant in the playback layer and a dozen
	literals inside the store's getters - five mechanisms for one idea, each of them a place to forget.

	Two rules for what belongs here:

	1. **A user-facing preference does.** If it has a key in `SettingsStore`, its default, bounds and
	   step are stated here once.
	2. **A protocol or hardware fact does not.** The 8 kHz sample reference, an ADPCM step table, a VGM
	   block type and the MIDI note range are properties of the formats, not preferences; they stay
	   with the code that speaks them.

	The namespaces below keep the shape the callers already use (`settings::keyboard::...`), so the
	single table did not need a second naming scheme.
*/
namespace midi
{

/** Semitones each way. 2 by default: these bends carry the chips' fine pitch - a rounding remainder,
	a vibrato, a slide - and a 14-bit bend spends its 8192 positive units over the range, so +/-2
	gives 4096 units per semitone where +/-12 gives 682. */
constexpr int minPitchBendRange = 1;
constexpr int maxPitchBendRange = 24;
constexpr int pitchBendRange = 2;

/** Off: a note keeps its nearest semitone and no bend is sent. On by default. */
constexpr bool pitchBendEnabled = true;

/** Whether a sampled voice also carries the root measured out of its sample. On by default since
	2026-11-09; off places every sampled voice by the playback rate against an 8 kHz convention. */
constexpr bool transposeBySampleRoot = true;

} // namespace midi

namespace keyboard
{

/** How long a key stays lit after its note stops.

	A chip's period register holds its last value, and a fast passage has gaps between notes far
	shorter than a display frame. Holding the key for a moment keeps the row from blinking without
	pretending the note is still sounding - the meter empties on time either way.
*/
constexpr int minNoteHoldMs = 0;
constexpr int maxNoteHoldMs = 500;
constexpr int noteHoldStepMs = 10;
constexpr int defaultNoteHoldMs = 120;

/** Which octave number MIDI 60 carries. The international convention calls it C4; this project
	starts at C3, which is what FL Studio and Yamaha call it, and the keyboard's labels follow this
	setting wherever they appear. */
constexpr int minMiddleCOctave = 2;
constexpr int maxMiddleCOctave = 5;
constexpr int defaultMiddleCOctave = 3;

/** The leftmost note the on-screen keyboard starts at; -1 means "follow the song". */
constexpr int defaultStartNote = -1;

/** Whether the keyboard draws the range hints under its keys. */
constexpr bool rangeHints = true;

} // namespace keyboard

namespace view
{

/** Which content view the window shows first: Mixer, then Keyboard, then Oscilloscope. */
constexpr int contentIndex = 0;

} // namespace view

namespace nsf
{

/** How long an NSF plays, in seconds.

	An NSF has no length: its driver loops until it is stopped, so this is the number the timeline,
	the seek bar and the end of a song are built on.

	The store clamps to these and the settings page draws its slider from them. They live together
	because a slider that can reach a value the store refuses snaps back without saying anything -
	which reads as a broken slider, not as a mismatch between two files. */
constexpr int minDurationSeconds = 10;
constexpr int maxDurationSeconds = 600;
constexpr int durationStepSeconds = 5;
constexpr int defaultDurationSeconds = 300;

/** Output gain in decibels.

	The NES mix table leaves a lot of headroom - five four-bit channels never approach full scale -
	so a file played unamplified sits eight to twelve decibels below a VGM of the same music. Eight
	is enough to bring the corpus level; the range goes to twenty-four so a quiet file can be lifted
	without leaving the page, at the cost of clipping the loudest ones. */
constexpr int minOutputGainDb = 0;
constexpr int maxOutputGainDb = 24;
constexpr int outputGainStepDb = 1;
constexpr int defaultOutputGainDb = 8;

/** Whether the chips mix their channels the way the hardware did.

	The NES adds its five channels through a measured non-linear curve rather than summing them:
	the trio of triangle, noise and DPCM passes through one table, and each channel's share of the
	result depends on what the other two are doing. That is what makes a real NES sound the way it
	does, and it is why a DPCM row in the split view carries a little of the triangle.

	Off, the channels add linearly. Each row then stands alone - clearer for reading a mix - at the
	cost of the compression the hardware applied, and with no headroom left for the sum of three
	full-scale channels.

	**Off by default, which is the opposite of what the hardware does.** Split tracks are what this
	player offers, and a scheme that makes one row depend on its neighbours reads as crosstalk
	until the explanation is found - the first person to hear it reported it as a bug, twice. The
	faithful mix is one switch away, and the page says what the switch costs.
*/
constexpr bool nonlinearMixer = false;

} // namespace nsf

namespace libvgm
{

/** **Whether a track the song never writes to gets no emulation.**

	A VGM declares its channels in its command stream, so the whole song can be read before any
	player exists. Most files use a fraction of what they declare: `07 Challenging Stage Clear`
	names 88 channels across three sample chips and plays three of them, and without this the
	player built all 88 - each with its own copy of the chip's ROM, 1.2 GB of address space spent
	on the other 85 producing silence.

	On, such a track gets a `SilentTrackPlayer` instead of an emulation. The mixer still lists the
	row, its mute, gain and pan still work, and its meter reads zero - the row is there, the
	emulation behind it is not.

	**How it decides, because this is the switch to turn off when it guesses wrong.** The event
	list is built at load time by replaying the command stream through each chip's decoder, and a
	track that never appears in it is taken to have nothing to play. That list is a *note* list,
	not a write log: a channel that holds one pitch, or only changes volume, can sound for the whole
	song and produce no event at all. It happened once - a decoder left a pulse marked muted after
	a sweep overflow - and a playing track was replaced with silence while the mixer and the keyboard
	page both kept showing it. That decoder is fixed, but the shape of the mistake is worth a switch:
	off, every declared channel gets an emulation, and the cost is memory a song may not need.
*/
constexpr bool skipSilentTracks = true;

} // namespace libvgm

namespace exporting
{

/** Loop the song for the whole export length rather than once. */
constexpr bool looped = false;

/** How many bits a PCM export writes: 0 keeps the engine's own format. */
constexpr int minExportBits = 0;
constexpr int maxExportBits = 32;
constexpr int defaultExportBits = 0;

/** Export length in seconds when the selection is left alone. */
constexpr int minExportSeconds = 1;
constexpr int maxExportSeconds = 10;
constexpr int defaultExportSeconds = 2;

/** The longest a single exported sample may be, in seconds; 0 means no limit.

	Five seconds is longer than any recording these chips are meant to hold (the longest ADPCM-A
	range on `07 Stage 2` is 3.4 s) and far shorter than a driver's "end at the end of the ROM", which
	is how a bank turns into a minute-long file. The limit is applied when the file is written, not
	when the samples are collected: changing it takes effect on the next export instead of needing
	the song to be loaded again. */
constexpr int minSampleSeconds = 0;
constexpr int maxSampleSeconds = 30;
constexpr int defaultSampleSeconds = 5;

/** **The rate samples are written at.**

	`sampleRateChip` keeps the rate the chip read the bytes at - the truth, and what a tracker
	reads. `sampleRateHost` holds each level at the rate the project runs at, so a file dropped
	into a 44.1/48 kHz session is played without resampling: resampling is band-limited to the
	file's own Nyquist and smooths an 8-bit DAC's staircase away, which is the difference between
	our wav and the same bytes in Furnace. 0 is the default because the export runs inside a host
	and usually lands in that host's timeline.
*/
constexpr int sampleRateChip = -1;
constexpr int sampleRateHost = 0;
constexpr int defaultSampleRate = sampleRateHost;

/** How long a wavetable dragged out of the window stays in the cache, in hours.

	Off by default, and the defaults are the point: deleting a file the user made is worse than
	keeping a few megabytes, so the cache only clears when someone has asked it to. A file dragged
	out but not yet imported is exactly what a cleaner would eat first.

	One hour is the floor because a lower number is a cleaner eating the file the user is dragging.
	A week is the ceiling because past that the number stops describing a cache.
*/
constexpr int minRetentionHours = 1;
constexpr int maxRetentionHours = 168;
constexpr int defaultRetentionHours = 3;

/** Whether the cache is cleared on start-up at all. Default off - see above. */
constexpr bool defaultCleanupEnabled = false;

} // namespace exporting

namespace fm
{

/** **How long an FM note is rendered for**, in seconds.

	Not derived from the envelope. The chip's rate codes reach a four-minute sweep at RR = 1, and a
	driver never uses those codes for a note it means to end - so the registers describe a sound
	nobody plays, and reading a length out of them gives an honest four minutes instead of a sample.

	Two seconds holds a note and its tail. The range is deliberately small: past five seconds the
	strip is showing silence, and below half a second there is no room for a release.
*/
constexpr int minSustainMs = 500;
constexpr int maxSustainMs = 10000;
constexpr int sustainStepMs = 100;
constexpr int defaultSustainMs = 1500;

/** **How much tail to keep after the key comes up**, in milliseconds.

	The patch does define a release rate, and its range is the problem: RR = 1 is a four-minute sweep
	on this chip. A driver never writes that for a note it means to end, so the register describes a
	sound nobody plays - the length is a preference instead, and this is its default.
*/
constexpr int minReleaseMs = 1;
constexpr int maxReleaseMs = 5000;
constexpr int releaseStepMs = 1;
constexpr int defaultReleaseMs = 100;

/** **The note an export renders at, when the user asks for a fixed one.**

	Off by default: an export of the row that is playing should sound like that row, and switching
	the pitch on is the deliberate act of asking for a different one. The range is a piano's - A0 to
	C8 - because the point of the control is to pick a playable note, not to sweep the register.
*/
/** **Whether a drawn note is scaled to fill the row.**

	On by default. A carrier at total level 48 is -36 dB by design, and drawn at that level the row is
	a flat line - the shape is what the window is showing, so the shape fills the range. The operator
	ratios inside the patch are untouched; this is one gain for the whole note.

	**The panel only.** An export carries the level the chip produced, because the gain is chosen from
	the note's own peak: a quiet patch comes up seventeen-fold, and residual aliasing and quantisation
	noise come up with it. A level that changes per note is also not a level - it is this drawing
	convention leaking into a file. See the export path in `ContentSlot`.

	**The old wording said "off by default" beside a `true`**, and it claimed to govern both paths
	long after it stopped.
*/
constexpr bool normalise = true;

constexpr int minFixedNote = 21;
constexpr int maxFixedNote = 108;
constexpr int defaultFixedNote = 60;

/** **Whether an export renders at the pitch the row is playing, or at the fixed note above.**

	On by default, because a chip's timbre depends on the pitch: key scaling sets the envelope's rate
	from the note, so a C4 render of an A2 note is not that note's sound. That is the answer a
	listener expects from "export this row".

	Off gives every export the fixed note, which is what comparing patches across a song wants - and it
	is what the panel always does, because a shape that re-renders on every note change costs the audio
	thread. Only the panel's fixed note is affected by `fm.note`; this decides whether export follows it.
*/
constexpr bool exportAtPlayedPitch = true;

} // namespace fm

namespace window
{

/** Reuse the running window when another song is opened, instead of starting a second one. */
constexpr bool reuseWindow = true;

/** Reopen the song that was playing when the application last closed. */
constexpr bool restoreLastSong = true;

/** Closing the window quits. A process that survives a close has to be asked for. */
constexpr bool closeToTray = false;

/** Print paint counters into the status line - a diagnostic, not something a listener wants. */
constexpr bool paintStats = false;

/** The Keyboard page's redraw rate, in hertz. */
constexpr int minRefreshHz = 10;
constexpr int maxRefreshHz = 60;
constexpr int defaultRefreshHz = 15;

} // namespace window

} // namespace toybox::settings
