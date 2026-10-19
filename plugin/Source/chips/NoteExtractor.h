#pragma once

#include <cstdint>
#include <vector>

#include "ChipTypes.h"
#include "IChipDecoder.h"

namespace toybox::chips
{

/** Turns a decoder's continuous channel state into discrete notes.

	The decoder runs over the whole write stream at a fixed grid; every grid point where a
	channel's gate opens or its pitch changes starts a note, every point where it closes ends
	one. Nothing here knows about NES, FM or PSG - a decoder only has to answer channelState().

	@param totalSamples the end of the stream, used to close notes still sounding
*/
std::vector<NoteEvent> extractNotes (IChipDecoder& decoder,
									 const std::vector<RegisterWrite>& writes,
									 std::uint32_t totalSamples);

} // namespace toybox::chips
