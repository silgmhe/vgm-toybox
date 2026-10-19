#pragma once

#include <cstdint>
#include <vector>

#include "ChipTypes.h"
#include "IChipDecoder.h"

namespace toybox::chips
{

/** What one pass over a write stream yields: the notes, and the pitch curve inside them. */
struct ExtractedPerformance
{
	std::vector<NoteEvent> notes;
	std::vector<PitchEvent> bends;
};

/** Turns a decoder's continuous channel state into discrete notes and pitch bends.

	The decoder runs over the whole write stream at a fixed grid; every grid point where a
	channel's gate opens or its pitch changes starts a note, every point where it closes ends
	one. Nothing here knows about NES, FM or PSG - a decoder only has to answer channelState().

	@param totalSamples the end of the stream, used to close notes still sounding
*/
ExtractedPerformance extractPerformance (IChipDecoder& decoder,
										 const std::vector<RegisterWrite>& writes,
										 std::uint32_t totalSamples);

/** The notes alone, for callers that do not reproduce vibrato. */
std::vector<NoteEvent> extractNotes (IChipDecoder& decoder,
									 const std::vector<RegisterWrite>& writes,
									 std::uint32_t totalSamples);

} // namespace toybox::chips
