#pragma once

namespace toybox::playback
{

/** What MIDI output can be derived from a mixer track.

	It lives in `midi/` rather than next to `TrackInfo`, which is the only thing that carries one:
	the question it answers - "can this channel produce notes, and of what kind" - is a property of
	the decoder, not of the mixer. Keeping it here is what lets a capability table be written and
	tested without a window, an audio device or a `juce::String` in sight.
*/
enum class MidiCapability
{
	unavailable,	// no MIDI decoder for this chip/channel yet
	melodic,		// the channel has a note and pitch (FM, PSG, square, ...)
	discrete,		// a note, but only from a fixed set of them - the noise channel's sixteen
					// periods, the slots a song gives its DPCM samples. The note is real enough
					// to play and to build a drum kit on, and carries no pitch to slide.
	oneShot			// the channel can only trigger a fixed note (ADPCM, DAC, ...)
};

} // namespace toybox::playback
