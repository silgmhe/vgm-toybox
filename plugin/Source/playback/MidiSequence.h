#pragma once

#include <cstdint>
#include <vector>

namespace toybox::playback
{

/** One event of the MIDI stream generated from a song. Times are relative to the start of one
	playthrough. The channel is deliberately not stored here: the user can change it per track
	without rebuilding the sequence. */
enum class MidiEventType
{
	noteOn,
	noteOff,
	pitchBend,
	controlChange
};

struct MidiEvent
{
	double timeSeconds = 0.0;
	MidiEventType type = MidiEventType::noteOn;
	std::uint8_t data1 = 0;		// note / controller / pitch-bend LSB
	std::uint8_t data2 = 0;		// velocity / value / pitch-bend MSB
	std::uint32_t sourceTrack = 0;
};

/** A read-only event list for one song. It is built while the song is loaded and never changed
	afterwards, so the audio thread can scan it without a lock. */
struct MidiSequence
{
	std::vector<MidiEvent> events;
	double loopStartSeconds = -1.0;
	double loopEndSeconds = -1.0;
};

} // namespace toybox::playback
