#pragma once

#include <cstdint>
#include <vector>

#include "playback/WaveExport.h"

namespace toybox::midi
{
struct SongIr;
}

namespace toybox::backends::libvgm
{

/** The Y8950's Delta-T samples, one file per range the driver starts.

	The Y8950 is the OPL2 with a sample player attached: registers `0x09/0x0A` and `0x0B/0x0C` name a
	start and a **last page**, `0x10/0x11` the pitch (DELTA-N), and `0x07` bit 7 starts it. The bytes
	come from a `0x88` block - VGM's "Y8950 DELTA-T ROM data" - which the reader already turns into
	`ir.roms`, so the collector only needs the registers.

	**One file per range**, deduplicated by `(start, length, DELTA-N)`: `05 Impact 151` keys on 807
	times and names five ranges of 4 KB each, which is the drum kit the song plays.

	**The rate is `delta * clock / (72 * 65536)`** - the OPL's own time base, not the OPN's 144. The
	same nibble format (Delta-T) is decoded by `chips/OpnAdpcmB.h`, so a file here and an OPN Delta-T
	file are the same kind of data at different clocks.
*/
std::vector<playback::DecodedSample> collectY8950Samples (const midi::SongIr& ir,
														  uint32_t clockHz = 0);

} // namespace toybox::backends::libvgm
