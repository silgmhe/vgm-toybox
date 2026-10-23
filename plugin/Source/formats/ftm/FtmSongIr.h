#pragma once

// FTM to APU register writes, for playback.
//
// The obvious route was to port FamiTracker's compiler and emit an NSF. That route is wrong for this
// project for a reason that has nothing to do with difficulty: there is no NSF player here. `SongIr`
// is a register-write stream and the only backend is libvgm, so an NSF would need a 6502 core and a
// driver to interpret it before a single sample came out.
//
// What playback actually needs is the thing the driver *does*, not the driver: per frame, decide each
// channel's period, volume and duty cycle, and write the APU registers. That is a few hundred lines
// of arithmetic here, testable against register writes, and it plugs into the existing pipeline with
// no new backend. Porting the compiler (the ISongSource/CChunk work) stays available for an export
// feature, but it is not on the path to sound.
//
// Unison with the tracker is the point, so the arithmetic follows the chip:
//   pulse    period = clock / (16 * frequency) - 1, eleven bits
//   triangle period = clock / (32 * frequency) - 1, eleven bits
//   noise    one of sixteen periods, chosen by note
// and the volume is a constant-level write (0x30 in $4000/$400C) rather than a hardware envelope,
// because the envelope is the instrument's job in this format and the driver does it in software.

#include <cstdint>
#include <string>
#include <vector>

#include "midi/SongIr.h"

namespace toybox::formats::ftm
{

/** The 2A03's NTSC clock. The module says NTSC or PAL; PAL is 1662607 and is not wired through yet,
	so a PAL module will play about 7% sharp rather than at the wrong speed - the same trade the
	loader's frame rate already makes. */
inline constexpr std::uint32_t nesApuClockHz = 1789773;

/** One channel's state between frames. Kept next to the emitter rather than inside it because the
	frame loop reads and writes all four fields and hiding them in a lambda would make the state
	harder to reason about than the arithmetic. */
struct ApuChannelState
{
	bool enabled = false;
	int note = -1;					// -1 when the channel is not sounding
	int instrument = 0;
	int level = 0;					// 0..15, what $4000/$400C will carry
	int duty = 2;					// 0..3, only pulse channels
	int macroStep = 0;				// position in the instrument's volume sequence

	// Row effects are state here, not events: they move the pitch every frame, and a note carries
	// them for as long as it sounds. The MIDI path turns them into bend events because that is what
	// MIDI has; this path keeps them as the numbers the chip needs each frame.
	int arpeggioX = 0;				// 0xy
	int arpeggioY = 0;
	int vibratoSpeed = 0;			// 4xy
	int vibratoDepth = 0;
	int portaUnits = 0;				// 1xx / 2xx, signed, in period units per frame
	int tonePortaUnits = 0;			// 3xx
	int effectFrame = 0;			// frames since the note started
	double periodOffset = 0.0;		// accumulated by the portamentos, in period units
	int lastPeriod = 0;				// the period the channel was sounding, for 3xx to start from
};

/** Emits the register writes a FamiTracker module would produce, frame by frame.

	Returned timestamps are in samples at 44,100 Hz, the unit the VGM loader already uses, so the
	assembly layer needs no special case: `tickRate` stays 44100 and a frame is 44,100 / frameRate
	samples long. Notes are not inferred here - nothing is decoded - so the stream is authored, the
	same way FTM's MIDI path is. */
bool loadSongIr (const void* data, std::size_t size, midi::SongIr& out, std::string& error);

/** The same writes, wrapped in a VGM image.

	This is how an FTM reaches the speakers without a second player: libvgm already plays VGM, and a
	VGM is a header plus a list of register writes with waits - which is exactly what `loadSongIr`
	produces. Serialising is a hundred lines against a new backend, and it keeps one playback path
	instead of two.

	The header declares version 1.51, the NES APU clock at 0x84, and a 0x100-byte data offset; the
	commands are 0x61 waits and 0xB4 APU writes, whose register byte is already an offset from
	$4000 - the numbering the emitter used. */
bool buildVgm (const void* data, std::size_t size, std::vector<std::uint8_t>& out, std::string& error);

/** The same, behind the loader interface the assembly layer uses. */
class FtmSongIrLoader final : public midi::ISongLoader
{
public:
	bool load (const void* data, std::size_t size, midi::SongIr& out, std::string& error) override
	{
		return loadSongIr (data, size, out, error);
	}
};

} // namespace toybox::formats::ftm
