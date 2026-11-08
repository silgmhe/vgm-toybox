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
/** The samples a Sega PCM song plays, one per distinct range it latched.

	A voice's registers *are* its playback state, but not where its bytes come from: a range is
	`(addressHigh << 8) | addressMid` up to `(endReg + 1) << 8`, both in bytes, and the bytes are
	eight-bit unsigned around 0x80 (`segapcm.c:182`). Keying on is a register edge - the control
	byte's bit 0 clears (`segapcm.c:154`) - so the ranges are collected at those edges and filed by
	content, which collapses a drum played thirty times into one file.

	`clockHz` turns a voice's pitch into a byte rate (`pitch * clock / 32768`); zero falls back to
	the 8 MHz the chip runs at on these boards.
*/
std::vector<playback::DecodedSample> collectSegaPcmSamples (const midi::SongIr& ir, uint32_t clockHz);

} // namespace toybox::backends::libvgm
