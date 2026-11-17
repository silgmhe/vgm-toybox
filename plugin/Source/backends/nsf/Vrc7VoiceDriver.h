#pragma once

#include <memory>

#include "playback/VoiceDriver.h"

namespace toybox::backends::nsf
{

/** **The NSF side of `IVoiceDriver`**: it drives NSFPlay's OPLL.

	The VRC7's FM is an OPLL with six voices and a cartridge's patch set, and the player already
	links it - `libs/nsfplay`, the same `emu2413.c` that `NES_VRC7` uses. A waveform drawn from a
	second copy of that code would be a second opinion about a patch; this one is the same opinion.

	`clockHz` is the cartridge's, from the file's header: pitch on this chip is a count of clocks,
	so a driver built for the wrong clock plays the right shape at the wrong speed.

	Returns a driver whatever the song contains: a file without a VRC7 simply never loads a patch
	into it, and `load` refusing is what tells the caller there is nothing to draw.
*/
std::unique_ptr<playback::IVoiceDriver> makeVrc7VoiceDriver (double clockHz);

} // namespace toybox::backends::nsf
