#pragma once

#include "playback/ChipState.h"

#include <string>

namespace toybox::playback
{

/**
	Writes a channel's waveform out as a WAV a sampler can take.

	The interesting decision here is the sample rate. A chip channel is a cycle of N samples that
	the hardware repeats at the note's frequency; writing it at 44100 would give a file whose pitch
	is "44100 / N" rather than the note that was playing. The rate is therefore chosen so that
	looping the cycle reproduces the original pitch: rate = frequency * length. When the frequency
	is unknown or the result is outside what a sampler will accept, it falls back to 44100 and the
	file is honest about being a shape rather than a note.

	Repeats exist because a single cycle of 32 samples is a legal WAV and an unusable sample: some
	samplers refuse it, and every one of them loops it badly.
*/
bool writeCycleWav (const ChannelState& channel, const std::string& path, int repeats = 16);

} // namespace toybox::playback
