#pragma once

#include <cstdint>

#include "MidiCapability.h"
#include "SongIr.h"

namespace toybox::midi
{

/** How a chip's channels answer: the first `melodicCount` carry a pitch, the next `oneShotCount`
	only a trigger, and the last `tailMelodicCount` carry a pitch again.

	The tail is what makes one shape cover every chip here. An OPN links an SSG behind itself and
	libvgm numbers the SSG's channels after the parent's, so a YM2608 has six FM channels, six
	ADPCM-A triggers, then ADPCM-B and the three SSG voices - melodic on both sides of the
	triggers. Without the third field the table would need a function per chip, which is the switch
	this replaces.

	Out-of-range indices, and whatever a plan leaves uncovered, are `unavailable`. That is the one
	answer that cannot make a promise the decoder will not keep.
*/
struct ChannelPlan
{
	int melodicCount = 0;
	int oneShotCount = 0;
	int tailMelodicCount = 0;

	/** What channel `index` offers. */
	playback::MidiCapability at (int index) const;
};

/** The plan for a chip, or an empty one when nothing is known about it.

	Keyed on `ChipKind` rather than on libvgm's `DEVID_*`, which is what this used to be keyed on.
	SongIr.h states the intent: "the point of the layer this header introduces is that the MIDI side
	stops depending on which file format produced the song". A capability table built out of device
	ids was that dependence, and it cost a channel its MIDI output: Ay8910Decoder arrived while the
	entry stayed `unavailable`, and nothing connected the two because they were keyed differently.

	Now a decoder and its plan are two entries in the same vocabulary, and the coverage check in
	midi/ChipPlansTest asserts that every kind with a decoder has a plan that promises something.
*/
ChannelPlan channelPlanFor (ChipKind kind);

} // namespace toybox::midi
