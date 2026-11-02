#pragma once

#include <vector>

#include "playback/WaveExport.h"

namespace toybox::midi
{
	struct SongIr;
}

namespace toybox::backends::libvgm
{
	/** Every PCM sample an RF5C68 or RF5C164 song plays, in the order the drivers started them.

		Both chips are the same device with a different memory model, so one collector serves both -
		the VGM device id is what differs, and the registers the driver writes are identical.

		The fifth way these chips name a sample. C352 said start and end, QSound start and an exclusive
		end, K053260 a length, K054539 a marker; this one says **only a start**, and a sample ends at a
		`0xFF` data byte - the same byte the core jumps back to the loop address on. Length is
		therefore scanned, and exact.

		One thing is unlike any of the others: the bytes are not a ROM block but a **PCM bank**
		(`0x67` blocks of type `0x02`), assembled by `memory::pcmBank` - the same function the runtime
		feeds `setRam` from. A collector that asked for a plain ROM would find nothing.
	*/
	std::vector<playback::DecodedSample> collectRf5cSamples (const midi::SongIr& ir);
}
