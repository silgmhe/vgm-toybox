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

} // namespace nsf

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

namespace audio
{

/** Remove the DC offset an ADPCM accumulator leaves behind. Off by default: it costs a filter per
	channel and the offset is not audible on its own. */
constexpr bool dcBlock = false;

} // namespace audio

} // namespace toybox::settings
