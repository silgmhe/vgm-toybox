/*
	The DAW sync mapping - "the host clock is the master, the song position is a pure function of
	it".

	The reason this feature exists at all: a VGM has no tempo, so it cannot be aligned to a DAW's
	bar grid by musical time. Instead the user measures the tempo outside the plugin, sets a cue
	point on the DAW timeline (the DJ way of finding the downbeat), and everything after that is
	arithmetic: song position = host position - cue.

	Because that mapping is pure, nothing here has to detect seeks, loops or scrubs: a jump is just
	a large error, and a host loop that comes back before the cue simply means "wait at zero again".

	No JUCE in this file - PlaybackEngine includes it, and the core test links it.
*/

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace toybox::playback
{

/** The song facts the mapping needs, independent of how the backend reports them.

	`durationSeconds` is the length *with the current loop setting* (a multi-loop song is longer
	than one pass); `singlePassSeconds` is one playthrough and never depends on it. The mapping
	needs both, plus the fade tail: libvgm reports the length *without* the fade, but keeps
	rendering for `fadeSeconds` after the last loop, and the tail is only there for songs that
	have a loop point. */
struct SongTimeline
{
	double durationSeconds = 0.0;
	double singlePassSeconds = 0.0;
	double loopLengthSeconds = 0.0;
	double fadeSeconds = 0.0;
	int loopCount = 1;
	bool loopsForever = false;

	bool hasLoop() const noexcept { return loopLengthSeconds > 1.0e-9; }
	double loopStartSeconds() const noexcept
	{
		return std::max (0.0, singlePassSeconds - loopLengthSeconds);
	}
	/** 0 means "forever" - the value libvgm uses, kept in one place. */
	int effectiveLoopCount() const noexcept { return loopsForever ? 0 : std::max (1, loopCount); }
	double totalSeconds() const noexcept
	{
		const int loops = effectiveLoopCount();
		return (loops == 0) ? singlePassSeconds
							: singlePassSeconds + (double) (loops - 1) * loopLengthSeconds;
	}
	/** How much audio follows `totalSeconds()` while the fade plays out. */
	double fadeTailSeconds() const noexcept
	{
		return (hasLoop() && effectiveLoopCount() != 0) ? std::max (0.0, fadeSeconds) : 0.0;
	}
	double endSeconds() const noexcept { return totalSeconds() + fadeTailSeconds(); }
};

/** Fills a SongTimeline from the backend's facts, repairing the cases a backend may not report
	(a missing single-pass length, a loop longer than the song). */
SongTimeline makeSongTimeline (double durationSeconds, double singlePassSeconds,
							   double loopLengthSeconds, int loopCount, double fadeSeconds,
							   bool loopsForever);

/** What the host said in the last block. `usable` is not the same as "the host sent a context":
	JUCE hands out an all-zero PositionInfo when there is none, so the caller has to decide - see
	the note on `publishHostClock()`. Seconds are the only unit here: the host's sample rate is a
	fact about the conversion the adapter does, not about the clock the mapping compares. */
struct HostClock
{
	bool usable = false;
	bool playing = false;
	double seconds = 0.0;
};

struct SyncDecision
{
	/** The engine should move its transport to `position` before rendering this block. */
	bool reposition = false;
	double position = 0.0;
	bool play = false;
};

/** How far the engine may drift before it is repositioned. A block at 48 kHz is 1-10 ms, so 30 ms
	is always larger than the natural error and smaller than anything audible.

	A parameter rather than a number buried in the function: how much drift is inaudible depends on
	the host's block size, and a test should be able to ask what happens at 0 ms. */
inline constexpr double kSyncToleranceSeconds = 0.030;

/** The pure part: where the song should be at the host's current time.

	`cueSeconds` is the host time at which the song's zero plays. Negative x means "the DAW has
	not reached the cue yet" - the song waits at zero, silent.

	The result is in the engine's own coordinate system, so that N >= 2 loops (where the engine
	counts across loops) and infinite loops (where it jumps back) both compare correctly. */
SyncDecision evaluateSync (const SongTimeline& timeline, const HostClock& host, double cueSeconds,
						   double enginePosition, bool enginePlaying,
						   double toleranceSeconds = kSyncToleranceSeconds);

/** The live state shared by the audio thread and the UI.

	Every value is an atomic: the audio thread publishes the host clock, the UI writes the cue and
	the knob values, and the audio thread reads them again. No locks - a block that loses a race
	sees the previous value and corrects itself on the next one. */
class DawSync
{
public:
	static constexpr double kMinKnobRangeMs = 1.0;
	static constexpr double kMaxKnobRangeMs = 2000.0;

	// --- the cue ---
	/** Where the song's zero sits on the host's timeline. It always exists: 0 - the DAW's start -
		is the default, and the menu's "reset" writes that value back rather than clearing anything.
		There is deliberately no "is a cue set" flag: Sync is the only switch, and a cue that is
		always there is arithmetic that always has an answer. */
	double getCueSeconds() const noexcept { return cueSeconds.load (std::memory_order_relaxed); }
	/** Sets the cue explicitly (used by Cue, by the menu's reset and by state restore); ignores
		non-finite and negative values. */
	void setCueSeconds (double seconds) noexcept;
	/** Reads the cue off the last published host clock. False when the host is not usable. */
	bool setCueFromHost() noexcept;

	// --- the offset knob ---
	double getCueOffsetMs() const noexcept { return cueOffsetMs.load (std::memory_order_relaxed); }
	void setCueOffsetMs (double ms) noexcept;
	double getKnobRangeMs() const noexcept { return knobRangeMs.load (std::memory_order_relaxed); }
	/** A new range re-clamps the offset, so the knob can never show a value it cannot reach. */
	void setKnobRangeMs (double ms) noexcept;

	/** cue + offset - what the mapping actually uses. */
	double getEffectiveCueSeconds() const noexcept
	{
		return getCueSeconds() + getCueOffsetMs() * 0.001;
	}

	// --- enable ---
	bool isEnabled() const noexcept { return enabled.load (std::memory_order_relaxed); }
	void setEnabled (bool value) noexcept { enabled.store (value, std::memory_order_relaxed); }

	// --- the host clock ---
	/** Called once per block on the audio thread. `usable` must already be decided by the caller:
		the first time a block reports "playing" latches the host as usable, so a host that only
		reveals itself while the transport runs does not lose the cue afterwards. */
	void publishHostClock (const HostClock& clock) noexcept;
	HostClock getHostClock() const noexcept;
	bool isHostUsable() const noexcept { return hostUsable.load (std::memory_order_relaxed); }
	bool isHostPlaying() const noexcept { return hostPlaying.load (std::memory_order_relaxed); }

	/** Everything needed to take over: the switch is on and the host reports time. No cue test -
		the cue always exists (see getCueSeconds()), so "on but not driving anything" is not a state
		the plugin can be in. */
	bool isActive() const noexcept
	{
		return isEnabled() && isHostUsable();
	}

	SyncDecision decide (const SongTimeline& timeline, double enginePosition,
						 bool enginePlaying) const noexcept
	{
		return evaluateSync (timeline, getHostClock(), getEffectiveCueSeconds(), enginePosition,
							 enginePlaying);
	}

private:
	std::atomic<bool> enabled { false };
	std::atomic<double> cueSeconds { 0.0 };	// 0 = the DAW's start, and the default
	std::atomic<double> cueOffsetMs { 0.0 };
	std::atomic<double> knobRangeMs { 10.0 };

	std::atomic<bool> hostUsable { false };
	std::atomic<bool> hostPlaying { false };
	std::atomic<double> hostSeconds { 0.0 };
	std::atomic<bool> seenHostPlaying { false };
};

} // namespace toybox::playback
