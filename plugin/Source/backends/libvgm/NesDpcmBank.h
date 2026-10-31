#pragma once

#include <cstddef>
#include <vector>

#include "playback/WaveExport.h"

namespace toybox::backends::libvgm
{

/** Every 2A03 DPCM sample a VGM's command stream started, decoded and ready to be written.

	Two things have to come together, and a VGM carries both. The bytes are in its `0x67` data
	blocks - a RAM-write block names the address its payload belongs at, which for this chip is
	somewhere in `$C000-$FFFF`. The samples themselves are named by the `$4012`/`$4013` writes that
	precede each `$4015` start, exactly as they are on hardware.

	A file that records the chip but not its ROM - no `0x67` blocks - yields nothing, and that is
	the honest answer: the bytes are not in the file, and walking it harder will not produce them.

	The whole stream is read once. That is the difference from the NSF path, which has to emulate
	the song to find out what it played; here the writes are already written down.
*/
std::vector<playback::DecodedSample> collectNesDpcmSamples (const void* vgm, std::size_t size);

} // namespace toybox::backends::libvgm
