#pragma once

#include "DawSync.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace toybox::playback
{

/** What the host's play head said this block, in the terms the sync follows.

	This is the only place that knows how the host's position is shaped, which is why it is here
	and not inside PluginProcessor.cpp: the rules below are the whole adapter, and they can be
	checked without a host (vgm_core_test, [11]).

	Two traps it has to handle: JUCE hands out *no* PositionInfo when the host has none (a
	standalone has no play head at all), and a host whose transport is parked may still send a
	context full of zeros. An all-zero clock would read as "the DAW is at 0 s", which the mapping
	turns into "before the cue, stay silent" - so a clock only counts as usable when it shows some
	sign of a real transport: playing, a non-zero time, a tempo or a position in quarter notes.

	`head` may be null. `localSampleRate` is this plugin's rate, used for the samples fallback and
	stored in the clock (see the .cpp for why that is only a fallback). */
HostClock readHostClock (const juce::AudioPlayHead* head, double localSampleRate);

} // namespace toybox::playback
