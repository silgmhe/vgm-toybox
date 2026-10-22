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
	gbPulse,
	gbWave,
	gbNoise,
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
	c6280Wave,		// HuC6280: a 32-step wavetable channel, pitch from a 12-bit divider
	sccWave,		// K051649/SCC: the same idea at 16 clocks per sample
	x1Wave,			// X1-010: a 128-byte wavetable stepped at clock * pitch / 2^26
	c140Pcm,		// C140: a 16-bit playback rate over recorded samples, root rate in Pitch.h
	c352Pcm,		// C352: the same idea with a 16-bit rate and an explicit key-on/key-off register
	k053260Pcm,		// K053260: a 12-bit pitch over a sample whose length is a register
	k054539Pcm,		// K054539: 8 voices, an attenuating volume and a sample that ends on a marker
	qsoundPcm,		// QSound: 16 voices whose loop is a length and whose silence is rate 0
	ga20Pcm,		// GA20: 4 voices, a rate divider and a sample that ends on a 0x00 byte
	wavetableWave,	// ES5503: 32 oscillators walking tables at freq * (clock/8/(n+2)) / 2^(9+res-size)
	rf5cPcm,		// RF5C68/164: 8 voices stepping a 64 KB RAM at step * (clock/384) / 2048 bytes a second
	okimAdpcm,		// OKIM6295: 4 voices at one fixed DAC rate, so the note is the sample's own cycle
	segaPcmPcm,		// Sega PCM: 16 voices, a step in 1/256 bytes and an end address in 256-byte units
	opnAdpcmB,		// YM2610/YM2608 ADPCM-B: a sampled voice with a real pitch register
	saaSquare,
	pokeySquare,	// distortion and clock mode change the timbre, not the note
	wsWave,			// WonderSwan: a 32-step waveform stepped at clock / (32 * (2048 - frequency))
	wsNoise,		// WonderSwan: the LFSR generator channel 3 can switch to
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
	/** The low byte of a sixteen-bit data register (the C352 is the only chip whose registers
		are written as whole words, and its VGM command carries both bytes). Zero everywhere else,
		where `value` alone is the write. */
	std::uint8_t value2 = 0;
	std::uint32_t sampleTime = 0;
};

/** What a decoder needs to know to start: the chip's clock, and the unit of the timestamps it will be told.

	This is the cure for the leak the review found in IChipDecoder::reset: the signature used to be
	(clock, sampleRate), with a comment saying the clock came "from the VGM header" - a file format
	inside the chip layer. A decoder needs a clock and a time base; where they came from is the loader's
	business. flags carries the board variants a chip needs (C140's banking). */
	struct ChipConfig
	{
		std::uint32_t clockHz = 0;			// 0 = unknown: the decoder falls back to its default
		double timeBaseHz = 44100.0;	// the unit of RegisterWrite::sampleTime
		std::uint32_t flags = 0;		// board / variant selection
	};

	/** Where a note's pitch came from - the honesty label the pitch model is built around.

	`registerExact` is a chip whose registers (plus documented hardware constants: a clock divider,
	a DAC rate, a delta-N scale) fully determine the frequency. `registerWithRoot` is the same plus
	a per-sample root correction - still from a documented constant in the end, but one that needed a
	root to place the octave. `sampleInferred` means the pitch had to be measured out of the sample's
	own bytes, which is the fallback for chips that give a rate but no reference (GA20, ES5503's
	multi-cycle tables) and is the part that can be wrong about the absolute octave. `unpitched` is
	no note at all: the C4 trigger a drum or an ambiguous sample reports. */
enum class PitchSource
{
	registerExact,
	registerWithRoot,
	sampleInferred,
	unpitched
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

	/** The pitch as the registers state it, in Hz. 0 when the channel has no pitch (a gate that is
		closed, a drum, a rate register of zero). */

	double hz = 0.0;

	/** L1: the transposition the registers ask for, in cents from the rate the chip's samples are
		taken to be recorded at. Always computable for a voiced channel, and the number the MIDI
		output uses unless the user asks for root correction (design doc section 14). */
	int transposeCents = 0;

	/** L2: the per-sample root correction in cents - 0 when the chip's reference rate is a
		documented constant (the default) or when the sample's root could not be measured. */
	int rootCents = 0;

	PitchSource pitchSource = PitchSource::unpitched;

	/** Monotonic count of key-on events on this channel, for the chips that have one.

		A driver that retriggers a voice at the same pitch - a bass line repeating a note, a drum
		hit on the same key - is starting a *new* note, but the sampled state alone cannot say so:
		the gate never dropped and the pitch never moved. The counter is what lets the extractor cut
		the previous note at the retrigger. Chips that do not fill it leave every note as it was. */
	std::uint32_t keyOns = 0;
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

	/** Both pitch layers travel with the note, so the "transpose by the sample's root" setting can
		be switched at render time instead of re-parsing the song (design doc section 14). `note` is
		already the L1 result; adding `rootCents` moves it by the sample's root. */
	int transposeCents = 0;
	int rootCents = 0;
	PitchSource pitchSource = PitchSource::unpitched;
};

/** A pitch inside a note, in cents from its nearest MIDI note. Vibrato and slides write the
	pitch registers every frame, so a note's pitch is a curve the note itself cannot carry. */
struct PitchEvent
{
	std::uint32_t sampleTime = 0;
	int logicalChannel = 0;
	std::int16_t cents = 0;
};

} // namespace toybox::chips
