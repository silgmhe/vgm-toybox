#pragma once

namespace toybox::playback
{

/**
	How a freshly loaded song's tracks are put on MIDI channels - the mixer card's MIDI shortcuts.

	The three schemes are **mutually exclusive**, not three independent switches: a track is either
	off, or on one channel, and "sequential" and "all on 1" cannot both describe it. The UI presents
	them as one choice for that reason (the mixer card's radio-style group).

	The default is `off`, which is also what `TrackState::midiChannel` has always started at: a
	plugin that sends nothing until asked cannot surprise a host, and a user who wants MIDI turns it
	on once and the choice is remembered with the project.
*/
enum class DefaultMidiChanneling
{
	/** Every track silent on MIDI (channel 0). The classic behaviour, and the default. */
	off,
	/** Track n -> channel (n % 16) + 1: track 1 plays on channel 1, track 17 wraps back to 1. */
	sequential,
	/** Every track on channel 1. */
	allToChannelOne
};

/** The channel `trackIndex` plays on under a scheme: 0 = Off, 1..16 as MIDI channel numbers.

	`trackIndex` is a position in the track list, not a note: the wrap at 16 is why a song with more
	than sixteen tracks still fits the protocol. A negative index has no channel to name, so it is
	off - an index that cannot happen is not worth a crash if it does.
*/
inline int midiChannelForTrack (DefaultMidiChanneling mode, int trackIndex)
{
	if (trackIndex < 0)
	{
		return 0;
	}

	switch (mode)
	{
	case DefaultMidiChanneling::sequential:      return (trackIndex % 16) + 1;
	case DefaultMidiChanneling::allToChannelOne: return 1;
	case DefaultMidiChanneling::off:             break;
	}

	return 0;
}

} // namespace toybox::playback
