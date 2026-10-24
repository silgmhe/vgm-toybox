#pragma once

#include <cmath>
#include <string>
#include <vector>

namespace toybox::playback
{

/**
	A snapshot of what the chips are doing, for display.

	This is deliberately a copy and not a view: the audio thread cannot be asked to lend out its
	state, and a display that reads live register arrays from another thread is a data race with a
	pretty face. Each render block ends by capturing this, and the UI reads the copy.

	It is also deliberately per-format rather than per-chip: a backend fills in whatever it can
	describe, and a panel renders whatever arrived. A backend that has nothing to say leaves the
	snapshot empty and the panel stays blank - which is the honest outcome, not a fabricated one.
*/
struct ChannelState
{
	std::string name;					// "Pulse 1", "Wave 3", ...
	std::string chipName;				// "2A03", "N163", ...

	bool active = false;				// key on / currently producing a tone
	int note = -1;						// semitones above C-1, -1 when silent
	float frequency = 0.0f;				// Hz, 0 when unknown
	float volume = 0.0f;				// 0..1, normalised against the chip's own maximum

	/** One period of the channel's waveform, values in -1..1. Empty for chips that generate
		their shape rather than read it from memory. */
	std::vector<float> wave;
};

struct ChipStateSnapshot
{
	std::vector<ChannelState> channels;
	double positionSeconds = 0.0;
	bool valid = false;
};

/** Semitones above C-1 for a frequency in Hz; -1 when the frequency is not usable. */
inline int noteForFrequency (float hz)
{
	if (! (hz > 0.0f))
	{
		return -1;
	}

	return (int) std::lround (12.0 * std::log2 ((double) hz / 440.0) + 69.0);
}

} // namespace toybox::playback
