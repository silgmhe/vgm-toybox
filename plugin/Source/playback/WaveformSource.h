#pragma once

#include <vector>

#include "playback/ChipState.h"

namespace toybox::playback
{

/** **The samples to draw or to write for this channel.**

	One function, two callers: the keyboard's window and the exporter both ask here, so a picture
	and a file cannot disagree about what this channel's waveform is. That failure has a history in
	this project - the View menu, the ADPCM image - and the reason the answer is a function rather
	than a field each caller reads for itself.

	Three sources, in the order a channel can offer them:

	1. `noteWaveform` - an FM note, whole envelope included;
	2. `wave` - one cycle of a wavetable, or a recording, as the chip gave it;
	3. `cycleFor` - a shape synthesised from the kind and duty, for a channel that reports neither.
*/
std::vector<float> waveformOf (const ChannelState& channel);

} // namespace toybox::playback
