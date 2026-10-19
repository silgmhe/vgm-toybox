#pragma once

#include <cstdint>
#include <vector>

namespace toybox::chips
{

/** Which sound source a channel belongs to - the vocabulary shared by every decoder, the note
	extractor and the encoder. Deliberately free of libvgm's DEVID_* values: the factory in
	backends/libvgm is the only place that knows the mapping. */
enum class Family
{
	nesPulse,
	nesTriangle,
	nesNoise,
	nesDmc,
	vrc6Pulse,
	vrc6Saw,
	vrc7Fm,
	fdsWave,
	mmc5Pulse,
	n163Wave,
	aySquare,
	ayNoise,
	ayEnvelope,
	sn76489Square,
	sn76489Noise,
	fm,
	fmDrum,
	adpcmDrum,
	unknown
};

/** One register write, in stream order, with its absolute position in 44100 Hz samples. */
struct RegisterWrite
{
	std::uint32_t deviceId = 0;		// libvgm instance id; routing only, never interpreted
	std::uint8_t reg = 0;
	std::uint8_t value = 0;
	std::uint8_t port = 0;			// second port / second chip of a pair
	std::uint32_t sampleTime = 0;
};

/** The normalised description of one channel at one moment. A decoder produces this; the note
	extractor turns a sequence of them into notes. Everything here is chip-agnostic on purpose:
	period stays in chip units and is resolved through the channel's note table. */
struct ChannelState
{
	bool present = false;			// the channel exists on this chip
	bool gate = false;				// key on: the channel is producing sound
	bool released = false;			// key off, but the envelope is still audible
	std::uint32_t period = 0;		// chip-native period / F-number (0 = silence)
	int block = 0;					// FM octave
	int volume = 0;					// 0..15, envelope already applied
	int patch = 0;					// duty / waveform / drum id
	std::int16_t finePitch = 0;		// cents outside the note table
};

/** A period-to-note lookup. Built per chip family, clock and tuning, because a period means
	different pitches on a 1.79 MHz CPU divider and on an FM chip. */
class NoteTable
{
public:
	/** Nearest MIDI note for a chip period, or -1 when the period is out of range. Sets
		finePitch to the distance from the table entry in cents. */
	virtual int noteFor (std::uint32_t period, int block, int* finePitch) const = 0;

	virtual ~NoteTable() = default;
};

/** One note as the encoder wants it. Times are absolute samples at 44100 Hz. */
struct NoteEvent
{
	std::uint32_t startSample = 0;
	std::uint32_t endSample = 0;
	std::uint8_t note = 60;
	std::uint8_t velocity = 100;
	int logicalChannel = 0;			// track / chip channel index inside the song
	Family family = Family::unknown;
	int patch = 0;
	bool isDrum = false;
};

} // namespace toybox::chips
