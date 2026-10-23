#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "chips/ChipTypes.h"

namespace toybox::midi
{

/** The notes a song contains, independent of where they came from.

	Two very different producers feed this: `inferNotes` (a decoder watching a register stream) and,
	later, a module loader that reads the notes the composer wrote. The MIDI encoder consumes only
	this type, so it never has to know which one it is holding - which is the whole point of the
	separation the design document calls A (registers) and B (notes).

	`track` is resolved by the producer: the register path asks the device table which MIDI track a
	chip channel belongs to, a module path reads its own channel list. The encoder therefore needs no
	device table and no chip vocabulary. */
struct StreamNote
{
	int track = -1;
	chips::NoteEvent note;
};

/** A pitch move inside a note - vibrato, a slide, a drum's pitch envelope. */
struct StreamBend
{
	int track = -1;
	chips::PitchEvent bend;
};

/** One track as the notes see it - a name and what kind of channel it was.

	Deliberately not `playback::TrackInfo`: that type carries a device id, an isolation flag and juce
	strings, all of which belong to the playback layer. A module knows only "channel 3, Triangle". */
struct StreamTrack
{
	std::string name;
	chips::ChannelKind kind = chips::ChannelKind::unknown;
	bool isDrum = false;
};

struct NoteStream
{
	std::vector<StreamNote> notes;
	std::vector<StreamBend> bends;
	std::vector<StreamTrack> tracks;

	/** The unit every sampleTime in the events is expressed in. Carried rather than assumed, for
		the same reason SongIr declares its own: a CPU-cycle format is not 44.1 kHz. */
	double tickRate = 44100.0;

	bool empty (void) const { return notes.empty() && bends.empty(); }
};

} // namespace toybox::midi
