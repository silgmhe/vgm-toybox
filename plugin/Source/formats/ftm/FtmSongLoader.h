#pragma once

#include <cstddef>
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
