#pragma once

#include <cmath>
#include <memory>
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

	/** The song is still being scanned, so this row's note is not known yet.

		Only the DPCM row can be in this state: its note is a sample slot, and a slot has to be
		numbered against the whole song before it means anything. The row can still be drawn -
		the shape comes from the address and rate, not from the slot - so this is a note-cell
		label rather than a blank row.
	*/
	bool analysing = false;
	float frequency = 0.0f;				// Hz, 0 when unknown

	/** For a channel that plays from a sample bank: the rate its bytes are read at, which is also
		the rate a file of them has to declare. Zero when this channel's shape is not a sample.

		Separate from `frequency` rather than sharing it: that field is a pitch and is cleared for
		the channels whose note is a numbering rather than a frequency, the DPCM among them, so a
		sample's rate has nowhere to live there.
	*/
	float sampleRate = 0.0f;

	/** How many sample points make one cycle of this channel's wave, or zero when unknown.

		It matters because `wave` holds the whole recorded sample, which can be thousands of points,
		while a waveform display has room for a couple of hundred. Downsampling all of it by peak
		would draw the sample's envelope - a shape that rises and falls but contains no period at
		all, which is what the C140 rows looked like before this existed. One cycle is what a
		waveform view is actually a picture of.

		Zero means "draw what there is": the DPCM's drums are short enough that the whole sample is
		the picture, and a chip that cannot measure a cycle says so by leaving this alone.
	*/
	float cycleBytes = 0.0f;

	float volume = 0.0f;				// 0..1, normalised against the chip's own maximum

	/** One period of the channel's waveform, values in -1..1. Null for chips that generate their
		shape rather than read it from memory - the display synthesises those from `waveKind`.

		Shared rather than held by value: a sampled channel's wave can be tens of kilobytes, and a
		snapshot is taken sixty times a second for every channel of every chip. Copying it into
		each snapshot would allocate and copy that much per frame. The decoder owns one copy for
		the lifetime of the state it describes, and the snapshot carries a reference to it.

		The pointer is to `const` on purpose: a consumer that could write through it would be able
		to change what a decoder believes about its own sample.
	*/
	std::shared_ptr<const std::vector<float>> wave;

	/** The wave as a reference, empty when the channel has none.

		Every reader wants the values, not the pointer, and every one of them would otherwise write
		the same null check. Returning a reference to a shared empty vector keeps the call sites
		reading as if the member were still a value.
	*/
	const std::vector<float>& waveValues (void) const
	{
		static const std::vector<float> none;

		return wave != nullptr ? *wave : none;
	}

	/** How the channel makes its shape, so a panel can draw it without knowing the chip:
		0 none, 1 stored table, 2 pulse, 3 triangle, 4 noise, 5 delta, 6 saw, 7 FM. */
	int waveKind = 0;

	/** Duty cycle for a pulse channel, 0..1. Ignored by the other kinds. */
	float duty = 0.5f;

	/** What the shape needs beyond its kind, in the channel's own units: the noise channel's
		period setting (0..15, the coarsest last), the DPCM channel's sample identity. Zero for
		the chips whose shape is fixed - a pulse's duty already carries everything a pulse has.

		Both exist because those two shapes are not fixed: a noise burst at period 0 is a fine
		hiss and at period 15 a slow rumble, and a kick and a hi-hat are two different samples.
		Drawing one generic mark for either would say nothing about what is playing.
	*/
	int shapeParameter = 0;

	/** Index into the engine's track list, or -1. A panel needs this to offer mute and solo:
		the engine already owns that state, and a second copy would drift. */
	int trackIndex = -1;
};

struct ChipStateSnapshot
{
	/** The chips this song declares, whether or not their channels can be described. A panel needs
		this to say "VRC6 is playing here and I cannot show it" instead of showing nothing. */
	std::vector<std::string> chips;

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
