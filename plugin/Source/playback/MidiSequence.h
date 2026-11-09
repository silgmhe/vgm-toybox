#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "settings/SettingsDefaults.h"

namespace toybox::playback
{

/** The range itself is a **user preference**, so it lives in `settings/SettingsDefaults.h` with every
	other default: `settings::midi::pitchBendRange` (2 semitones) and its bounds. The encoder still
	clamps with those bounds - it has to obey whatever number reaches it - and this file includes the
	defaults header rather than restating them. That header is a leaf with no dependencies, so the
	playback layer does not end up depending on the settings store. */

/** The 14-bit pitch-bend value (0..16383, 8192 = centred) for `cents` of bend at
	`rangeSemitones` each way. Returns -1 when no message should go out (`enabled == false`): the
	notes keep their nearest semitone and the fine pitch is dropped. */
inline int encodePitchBend (int cents, int rangeSemitones, bool enabled)
{
	if (! enabled)
	{
		return -1;
	}

	const int range = std::clamp (rangeSemitones, settings::midi::minPitchBendRange, settings::midi::maxPitchBendRange);
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
	uint8_t data1 = 0;		// note / controller (unused by pitchBend)
	uint8_t data2 = 0;		// velocity / value (unused by pitchBend)
	/** pitchBend only: how far the note is bent, in cents, from the note's own pitch. The
		sequence stores pitch, not MIDI bytes, because the 14-bit scale depends on a setting the
		song does not know (the receiver's range - see encodePitchBend). */
	int16_t bendCents = 0;

	/** noteOn only: the two pitch layers the note came from, carried so the pitch report can say
		which notes are register-exact and which needed a measured root (design doc section 15). They
		are not sent anywhere - `data1` is already the played note. */
	int16_t transposeCents = 0;
	int16_t rootCents = 0;
	uint8_t pitchSource = 0;	// chips::PitchSource

	uint32_t sourceTrack = 0;
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
