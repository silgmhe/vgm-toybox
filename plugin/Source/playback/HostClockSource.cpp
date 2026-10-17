#include "HostClockSource.h"

#include <cmath>

namespace toybox::playback
{

HostClock readHostClock (const juce::AudioPlayHead* head, double localSampleRate)
{
	const double rate = (localSampleRate > 0.0) ? localSampleRate : 48000.0;

	HostClock clock;

	if (head == nullptr)
	{
		return clock;
	}

	const auto position = head->getPosition();
	if (! position.hasValue())
	{
		return clock;
	}

	const auto timeSeconds = position->getTimeInSeconds();
	const auto timeSamples = position->getTimeInSamples();
	const auto bpm = position->getBpm();
	const auto ppq = position->getPpqPosition();

	clock.playing = position->getIsPlaying();

	// A tempo alone is enough: a host with a transport that is stopped usually still reports one,
	// and taking the cue has to work while the DAW is parked - that is the normal way to line a
	// song up with a bar.
	clock.usable = clock.playing || (timeSeconds.hasValue() && *timeSeconds != 0.0)
				   || (timeSamples.hasValue() && *timeSamples != 0) || (bpm.hasValue() && *bpm > 0.0)
				   || ppq.hasValue();

	// Seconds are the unit that survives everything: the samples are counted at the *host's* rate,
	// which is not necessarily this plugin's (a host may run it through a sample-rate converter),
	// while a time in seconds means the same thing on both sides. So samples are only the fallback
	// for a host that sends nothing else, and the conversion below uses our rate - close enough to
	// place a cue, not exact enough to hold a long song in sync, which is why it is the fallback.
	if (timeSeconds.hasValue())
	{
		clock.seconds = *timeSeconds;
	}
	else if (timeSamples.hasValue())
	{
		clock.seconds = (double) *timeSamples / rate;
	}

	// A nonsense time is not a time: without this the mapping would seek to it (the published
	// clock is finite-checked downstream too, but the state here has to agree with itself).
	if (! std::isfinite (clock.seconds))
	{
		clock.seconds = 0.0;
		clock.usable = false;
	}

	return clock;
}

} // namespace toybox::playback
