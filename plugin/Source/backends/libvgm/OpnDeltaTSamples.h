#pragma once

#include <vector>

#include "playback/WaveExport.h"

namespace toybox::midi
{
struct SongIr;
}

namespace toybox::backends::libvgm
{

/** Every distinct Delta-T (OPN ADPCM-B) recording the song starts.

	The sibling of `collectOpnAdpcmSamples()`, and different in three ways that matter:

	1. **The address page differs per board.** The OPNB's start/end registers count 256-byte pages
	   (shift 8), the OPNA's 32-byte ones (shift 5); `OpnDecoder::addressShiftFor()` is the rule and
	   this uses it rather than writing an 8 and a 5 here.
	2. **The block differs per board.** `0x83` on the OPNB, `0x81` on the OPNA, which often ships only
	   the ADPCM-A block - so the OPNA falls back to `0x82` through `sampleBlocksFor()`.
	3. **DELTA-N is a pitch register.** The same recording is played at many rates, so the file is
	   written at the **reference** DELTA-N (`0x4000`), i.e. untransposed; a file per playback rate
	   would duplicate the same bytes once per note. What the song does with it is a transposition,
	   not a different sample.

	`clockHz` is the clock the device was actually started with, the same fallback `collectOpnAdpcm-
	Samples()` takes: the IR's slot carries what the file declared, and a song that declares nothing
	would otherwise be measured against 8 MHz even on a 7.987 MHz OPNA.

	A range longer than 30 seconds at that reference rate is not a sample but a bank the driver
	pointed `end` at; it is skipped rather than exported as a minute-long file. The user-facing cap in
	Settings -> Export is applied on top of that when the file is written.
*/
std::vector<playback::DecodedSample> collectOpnDeltaTSamples (const midi::SongIr& ir,
														  uint32_t clockHz = 0);

} // namespace toybox::backends::libvgm
