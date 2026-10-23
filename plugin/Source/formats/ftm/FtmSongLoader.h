#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "midi/NoteStream.h"

namespace toybox::formats::ftm
{

/** One row of a pattern. A row can carry a note, an effect, or both - an effect-only row still
	matters, because `Bxx`/`Dxx` are the song's structure and usually sit on rows with no note. */
struct ModuleRow
{
	int row = 0;
	int note = 0;			// 1..12 = C..B, 0 = empty, above 12 = release/cut
	int octave = 0;			// 0..7
	int instrument = 0;
	int volume = 0;

	/** On-disk effect codes, which are FamiTracker 0.5.0's order (the conversion only permutes
		the expansion-chip tail, so the core ones keep their indices): 2 = jump, 3 = skip,
		4 = halt. */
	std::vector<std::pair<int, int>> effects;

	bool hasNote (void) const { return note != 0; }
};

struct ModulePattern
{
	int channel = 0;
	int index = 0;
	std::vector<ModuleRow> rows;
};

/** A FamiTracker module as the file states it - no MIDI, no chip model.

	The point of this type is what it does *not* contain: no register writes and no inference. A
	module names its notes, so the pipeline can take the authored path (design doc §18). */
struct Module
{
	std::string title;
	std::string author;

	int channels = 0;
	int machine = 0;		// 0 = NTSC, 1 = PAL
	int speed = 6;			// rows per frame
	int tempo = 150;		// frames per second * 2.5
	int patternLength = 64;
	int frameCount = 0;

	std::vector<int> channelTypes;					// 2A03: 0/1 pulse, 2 triangle, 3 noise, 4 DPCM
	std::vector<int> effectColumns;					// per channel, from the header
	std::vector<std::vector<int>> order;			// [channel][frame] -> pattern index
	std::vector<ModulePattern> patterns;

	/** One DPCM sample: a name and the bytes the chip plays. */
	struct DpcmSample
	{
		std::string name;
		std::vector<std::uint8_t> data;
	};

	/** Which sample a note triggers, at what rate, with which delta counter value. Mirrors the
		fields the driver reads out of its own table; -1 means the file did not carry a delta. */
	struct DpcmAssignment
	{
		int sample = 0;
		int pitch = 0;
		int delta = -1;
	};

	/** What an instrument says about itself. Index 0 means "no instrument" in FamiTracker, so the
		table is sparse and an entry whose sequences are all -1 is a real, silent instrument. */
	struct Instrument
	{
		std::string name;
		int volume = -1;		// SEQ_VOLUME: the level the note starts at
		int arpeggio = -1;		// SEQ_ARPEGGIO: semitone offsets, stepped one per frame
		/** SEQ_PITCH: fine detune. Parsed, not applied - the unit is genuinely unknown here, and
			the one number the reference makes easy to find (`m_iPitch`, clamped at 511) turned out to
			be *the MIDI pitch wheel offset*, not this macro (ChannelHandler.h:104). Guessing a scale
			would bend every note that uses the macro by a wrong constant and report nothing. */
		int pitch = -1;
		int duty = -1;			// SEQ_DUTYCYCLE: no MIDI equivalent, kept for a future audio path

		/** The per-note sample assignment, octave-major: `dpcm[octave][note]`. */
		std::array<std::array<DpcmAssignment, 12>, 8> dpcm {};

		/** FamiTracker's inst_type_t: 1 = 2A03, 2 = VRC6, 3 = VRC7, 4 = FDS, 5 = N163, 6 = S5B. */
		int type = 0;

		/** N163 only: the wave, four bits per sample, and where the tracker put it in the chip's
			128-byte RAM. `wavePos` is a nibble address - the hardware counts four-bit samples, which
			is why the RAM byte is `wavePos >> 1`. */
		int waveSize = 0;
		int wavePos = 0;
		std::vector<std::uint8_t> wave;

		/** VRC7 only: the patch. The chip is an OPLL, so a patch is either a preset number or, when
			the module says so, eight register bytes written before the note. */
		int vrc7Patch = 0;
		std::array<std::uint8_t, 8> vrc7Regs {};
	};

	/** The module's DPCM samples, indexed the way the instrument assignments refer to them. */
	std::vector<DpcmSample> samples;

	std::vector<Instrument> instruments;
	/** SEQ_VOLUME sequences, indexed the way the instrument table refers to them. */
	std::vector<std::vector<std::uint8_t>> volumeSequences;
	/** SEQ_ARPEGGIO sequences, in the same index space. */
	std::vector<std::vector<std::uint8_t>> arpeggioSequences;
	/** SEQ_PITCH and SEQ_DUTYCYCLE: parsed so the instrument table is complete, not yet applied. */
	std::vector<std::vector<std::uint8_t>> pitchSequences;
	std::vector<std::vector<std::uint8_t>> dutySequences;

	/** Rows per second: FamiTracker's tempo is 2.5 frames per unit, and `speed` rows pass per
		frame. At the defaults (150, 6) that is 10 rows a second. */
	double framesPerSecond (void) const;
};

/** Reads the container and the four blocks a note transcription needs.

	Only PARAMS, INFO, HEADER, FRAMES and PATTERNS are read; instruments, sequences and DPCM
	samples are skipped, which is why a module with no instruments still transcribes. */
bool parseModule (const void* data, std::size_t size, Module& out, std::string& error);

/** The authored path: a module becomes a `NoteStream` with no decoder involved.

	Tracks are the module's channels; times are in frames (see `Module::framesPerSecond`), which is\n\tthe grid FamiTracker's own effects step on. */
bool loadNoteStream (const void* data, std::size_t size, midi::NoteStream& out, std::string& error);

} // namespace toybox::formats::ftm
