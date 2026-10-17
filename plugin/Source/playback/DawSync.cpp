/*
	The DAW sync mapping - see DawSync.h for what this is and why it is one-way.
*/

#include "DawSync.h"

namespace toybox::playback
{

namespace
{
	/** The offset knob is clamped to its range; everything that writes it goes through here. */
	double clampOffset (double ms, double rangeMs) noexcept
	{
		const double limit = std::min (std::max (rangeMs, DawSync::kMinKnobRangeMs),
									   DawSync::kMaxKnobRangeMs);
		return std::min (std::max (ms, -limit), limit);
	}
}

SongTimeline makeSongTimeline (double durationSeconds, double singlePassSeconds,
							   double loopLengthSeconds, int loopCount, double fadeSeconds,
							   bool loopsForever)
{
	SongTimeline timeline;
	timeline.durationSeconds = std::max (0.0, durationSeconds);
	timeline.loopLengthSeconds = std::max (0.0, loopLengthSeconds);
	timeline.fadeSeconds = std::max (0.0, fadeSeconds);

	const bool forever = loopsForever || loopCount <= 0;
	timeline.loopsForever = forever;
	timeline.loopCount = forever ? 0 : loopCount;

	double singlePass = std::max (0.0, singlePassSeconds);
	if (singlePass <= 0.0)
	{
		// A backend that does not report the single pass (the fake one used to) still has a length
		// in `durationSeconds`; with N loops that length contains N-1 repeats, so take them out.
		singlePass = timeline.durationSeconds;
		if (timeline.loopLengthSeconds > 0.0 && ! forever && timeline.loopCount > 1)
		{
			singlePass = std::max (0.0, singlePass - (double) (timeline.loopCount - 1)
														 * timeline.loopLengthSeconds);
		}
	}

	timeline.singlePassSeconds = singlePass;

	// A loop longer than the song would make loopStartSeconds() clamp to 0 and the mapping jump
	// to the top on every repeat; treat it as "no loop" instead.
	if (timeline.loopLengthSeconds > timeline.singlePassSeconds)
	{
		timeline.loopLengthSeconds = 0.0;
	}

	return timeline;
}

SyncDecision evaluateSync (const SongTimeline& timeline, const HostClock& host, double cueSeconds,
						   double enginePosition, bool enginePlaying, double toleranceSeconds)
{
	SyncDecision decision;

	if (! host.usable || ! std::isfinite (host.seconds) || ! std::isfinite (cueSeconds))
	{
		// Without a trustworthy clock the only safe thing is to leave the engine alone.
		return decision;
	}

	const double x = host.seconds - cueSeconds;
	const double total = timeline.totalSeconds();
	const double end = timeline.endSeconds();

	double position = 0.0;
	bool play = false;

	if (x < 0.0)
	{
		position = 0.0;
		play = false;
	}
	else if (! timeline.hasLoop() || timeline.effectiveLoopCount() == 1)
	{
		position = std::min (x, end);
		play = x < end;
	}
	else if (timeline.effectiveLoopCount() == 0)
	{
		// Infinite loops: the engine counts inside one playthrough and jumps back at the loop
		// point, so the mapping has to wrap as well.
		position = (x < timeline.singlePassSeconds)
					   ? x
					   : timeline.loopStartSeconds()
							 + std::fmod (x - timeline.singlePassSeconds, timeline.loopLengthSeconds);
		play = true;
	}
	else
	{
		// N >= 2: the engine keeps counting across the repeats, so the position grows past the
		// end of a single pass without wrapping.
		position = std::min (x, end);
		play = x < end;
	}

	if (! host.playing)
	{
		play = false;
	}

	// The engine cannot be seeked into the fade tail - its own seek clamps to the reported length
	// (which excludes the fade) - and it plays the tail without being told to. So the target is
	// clamped to the seekable length, and the *comparison* happens in the same clamped space;
	// otherwise every block inside the tail would look like a 3-second error and seek again.
	const double longest = std::max (0.0, total);
	const double clampedPosition = std::min (std::max (0.0, position), longest);
	const double clampedActual = std::min (std::max (0.0, enginePosition), longest);

	decision.position = clampedPosition;
	decision.play = play;
	decision.reposition = (play != enginePlaying)
						  || (std::abs (clampedPosition - clampedActual) > toleranceSeconds);

	return decision;
}

void DawSync::setCueSeconds (double seconds) noexcept
{
	if (! std::isfinite (seconds) || seconds < 0.0)
	{
		return;
	}

	cueSeconds.store (seconds, std::memory_order_relaxed);
}

bool DawSync::setCueFromHost() noexcept
{
	const HostClock clock = getHostClock();
	if (! clock.usable || ! std::isfinite (clock.seconds))
	{
		return false;
	}

	setCueSeconds (clock.seconds);
	return true;
}

void DawSync::setCueOffsetMs (double ms) noexcept
{
	if (! std::isfinite (ms))
	{
		return;
	}

	cueOffsetMs.store (clampOffset (ms, getKnobRangeMs()), std::memory_order_relaxed);
}

void DawSync::setKnobRangeMs (double ms) noexcept
{
	if (! std::isfinite (ms))
	{
		return;
	}

	const double range = std::min (std::max (ms, kMinKnobRangeMs), kMaxKnobRangeMs);
	knobRangeMs.store (range, std::memory_order_relaxed);
	cueOffsetMs.store (clampOffset (getCueOffsetMs(), range), std::memory_order_relaxed);
}

void DawSync::publishHostClock (const HostClock& clock) noexcept
{
	const bool usable = clock.usable && std::isfinite (clock.seconds);
	if (usable && clock.playing)
	{
		// Latch: a host that only reveals its play head while the transport runs keeps the cue
		// usable after it stops again.
		seenHostPlaying.store (true, std::memory_order_relaxed);
	}

	// Only a usable clock may move the time: a host that stops sending a play head (instead of
	// sending a stopped one) would otherwise drop the time to zero and yank the song back to its
	// beginning. Keeping the last known value means "paused where it was", which is closer.
	if (usable)
	{
		hostSeconds.store (clock.seconds, std::memory_order_relaxed);
	}

	hostPlaying.store (clock.playing, std::memory_order_relaxed);
	hostUsable.store (usable || seenHostPlaying.load (std::memory_order_relaxed),
					  std::memory_order_relaxed);
}

HostClock DawSync::getHostClock() const noexcept
{
	HostClock clock;
	clock.usable = hostUsable.load (std::memory_order_relaxed);
	clock.playing = hostPlaying.load (std::memory_order_relaxed);
	clock.seconds = hostSeconds.load (std::memory_order_relaxed);
	return clock;
}

} // namespace toybox::playback
