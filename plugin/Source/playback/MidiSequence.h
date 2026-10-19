#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace toybox::playback
{

/** The pitch-bend range the MIDI output assumes, in semitones each way.

	Nothing in MIDI reports it: the range is a setting of the receiving instrument, changed with
	RPN 0 or by hand, and the host does not pass it on. So the plugin carries it as a preference
	(defaulting to the wide 12 most software synths use), scales every bend by it, and declares it
	with RPN 0 whenever playback (re)starts - an instrument that honours RPN then follows the
	setting, one that does not has to be set to the same value by hand. A mismatch is not a small
	error: a 50 cent bend encoded for +/- 2 semitones plays as 3 semitones on a +/- 12 instrument. */
inline constexpr int defaultPitchBendRange = 12;
inline constexpr int minPitchBendRange = 1;
inline constexpr int maxPitchBendRange = 24;

/** The 14-bit pitch-bend value (0..16383, 8192 = centred) for `cents` of bend at
	`rangeSemitones` each way. Returns -1 when no message should go out (`enabled == false`): the
	notes keep their nearest semitone and the fine pitch is dropped. */
inline int encodePitchBend (int cents, int rangeSemitones, bool enabled)
{
	if (! enabled)
	{
		return -1;
	}

	const int range = std::clamp (rangeSemitones, minPitchBendRange, maxPitchBendRange);
	const long units = std::lround ((double) cents * 8192.0 / (double) (range * 100));
	return 8192 + (int) std::clamp<long> (units, -8192, 8191);
}

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
	std::uint8_t data1 = 0;		// note / controller (unused by pitchBend)
	std::uint8_t data2 = 0;		// velocity / value (unused by pitchBend)
	/** pitchBend only: how far the note is bent, in cents, from the note's own pitch. The
		sequence stores pitch, not MIDI bytes, because the 14-bit scale depends on a setting the
		song does not know (the receiver's range - see encodePitchBend). */
	std::int16_t bendCents = 0;
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
