#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace toybox::playback
{

/** The pitch-bend range the MIDI output assumes, in semitones each way.

	Nothing in MIDI reports it: the range is a setting of the receiving instrument, changed with
	RPN 0 or by hand, and the host does not pass it on. So the plugin carries it as a preference,
	scales every bend by it, and declares it with RPN 0 whenever playback (re)starts - an instrument
	that honours RPN then follows the setting, one that does not has to be set to the same value by
	hand. A mismatch is not a small error: a 50 cent bend encoded for +/- 2 semitones plays as 3
	semitones on a +/- 12 instrument.

	**Two, because that is what these bends are for.** The MIDI here carries the chips' fine pitch -
	the remainder a sampled note's rounding leaves, a vibrato, a slide - and nothing wider than a
	semitone or two; what these songs do need is resolution. A 14-bit bend spends its 8192 positive
	units over the range, so +/- 2 gives 4096 units per semitone where +/- 12 gives 682: the same
	50 cents is 1024 units or 171, and the coarse one is audibly stepped. Widening the range is a
	matter of matching the instrument, not of hearing more. */
inline constexpr int defaultPitchBendRange = 2;
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
