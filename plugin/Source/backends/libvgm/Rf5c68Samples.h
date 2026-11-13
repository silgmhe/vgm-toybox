#pragma once

#include <vector>

#include "playback/SongBackend.h"
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

		And a second thing, which took much longer to notice: **that bank is only the load-time
		state.** A `0x67` block of type `0xC1` is a RAM write - a 16-bit address and the bytes that
		belong there, applied at the block's position in the command stream. Sonic CD's driver
		streams 512-byte pages into the first four kilobytes throughout the song (106 of them on one
		track), and a collector that ignores them exports the initial contents while the chip plays
		the rewritten ones. Every "the export is not what I hear" report on this chip was that.
	*/
	std::vector<playback::DecodedSample> collectRf5cSamples (const midi::SongIr& ir);

	/** **What the chip actually played, recorded from its output.**

		Everything else in this file reasons from the register stream, and on this chip that reasoning was
		wrong four times: the RAM is refilled while a voice plays, the same address holds different
		samples, and the driver gates voices in bursts of tens of milliseconds. Recording the audio
		side-steps all of it - the mask isolates one voice (measured: `none=3776, ch0-muted=0, all=0`),
		and the bursts are found on the waveform, which is the only boundary a listener can confirm.

		Costs one whole-song render per channel, so this is the slow path; it is also the honest one.
	*/
	std::vector<playback::DecodedSample> renderRf5cSamples (const playback::SongMemory& memory);
}
