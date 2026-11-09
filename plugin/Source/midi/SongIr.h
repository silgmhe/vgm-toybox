#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "chips/ChipTypes.h"

namespace toybox::midi
{

/** Which sound chip a write is for, in this project's own vocabulary.

	Deliberately not libvgm's `DEVID_*`: the point of the layer this header introduces is that the
	MIDI side stops depending on which file format produced the song. A VGM loader maps its command
	numbers onto these values; an NSF loader for the 2A03 would emit `nesApu` and nothing else would
	have to change. One value per decoder in `chips/`, so adding a chip stays a two-file operation. */
enum class ChipKind
{
	unknown,

	// --- square-wave PSGs and their noise channels ---------------------------------------
	ay8910,			// AY-3-8910 / YM2149: three squares, noise and envelope
	sn76489,		// SN76489 / SN76496
	saa1099,		// Philips SAA1099
	pokey,			// Atari POKEY

	// --- Yamaha FM, one value per device so the assembly layer can find the right one -----
	ym2413,			// OPLL
	ym3812,			// OPL2
	ym3526,			// OPL
	y8950,			// OPL + ADPCM (the FM half; the ADPCM half is an independent decoder)
	ymf262,			// OPL3
	ymf278b,		// OPL4's OPL3 half
	ym2612,			// OPN2
	ym2203,			// OPN
	ym2608,			// OPNA
	ym2610,			// OPNB
	ym2151,			// OPM
	ymf271,			// OPX
	ymz280b,		// a sample player with a real pitch register

	// --- sample players ----------------------------------------------------------------
	c140,			// C140 and the 219 ASIC, sixteen voices
	c352,			// C352, 32 voices, word-wide registers
	es5503,			// ES5503, 32 oscillators over wavetables
	ga20,			// GA20 / Y8950's ADPCM half
	k053260,		// Konami PCM with KADPCM
	k054539,		// Konami eight-voice PCM
	multiPcm,		// YMW258 / MultiPCM, 28 voices behind three ports
	okim6295,		// OKI ADPCM, four voices at one DAC rate
	qsound,			// Capcom QSound, 16 voices
	rf5c68,			// RF5C68
	rf5c164,			// RF5C164 (same device id, different memory model)
	segapcm,			// Sega PCM, 16 voices

	// --- wavetable and other ------------------------------------------------------------
	gameBoy,		// DMG APU
	huc6280,		// PC Engine
	k005289,		// Konami, two wavetable voices
	k007232,		// Konami, two PCM voices
	k051649,		// SCC
	mikey,			// Atari Lynx
	msm5232,		// OKI, the register is a key index
	nesApu,			// 2A03
	vsu,			// Virtual Boy
	wonderswan,
	x1_010,			// Seta X1-010
};

/** One chip in the song, as the loader found it.

	`instance` distinguishes the two halves of a paired chip (the second AY of a dual-AY rip, the
	second YM2612 of a 32X set). `clockHz` is what the file declared, 0 when it declared nothing, and
	`flags` carries a board variant a chip needs (C140's banking, for example). */
struct ChipSlot
{
	ChipKind kind = ChipKind::unknown;
	int instance = 0;
	uint32_t clockHz = 0;
	uint32_t flags = 0;
};

/** A ROM or RAM block the song carries.

	The fields mirror what a memory image needs and nothing else: a ROM block declares the size of
	the whole ROM and where its payload belongs, a RAM block declares only the address it writes at.

	`data` points into the file image the loader was handed - the IR does not own it. That is
	deliberate: a VGM can carry 32 MB of ROM, and copying it here and again into the assembled image
	would double the peak for no gain. The IR is built and consumed inside one call, so the view is
	valid throughout; a caller that wants to keep it must copy. */
struct RomBlock
{
	int memoryType = 0;			// the loader's numbering; each chip interprets its own
	uint32_t romSize = 0;	// bytes of the ROM this block belongs to (ROM blocks only)
	uint32_t romOffset = 0;	// where in that ROM the payload belongs (ROM blocks only)
	uint32_t ramOffset = 0;	// where in the chip's own RAM (RAM blocks only)
	const uint8_t* data = nullptr;
	std::size_t size = 0;
	uint32_t streamOrder = 0;	// position in the source file, same counter as the writes. A RAM
								// block matters at the moment the stream reaches it - before
								// that the bytes it overwrites are still the old ones.

	/** True when the block addresses the chip's RAM rather than a ROM image. */
	bool isRam = false;
	};

	/** One `0x68` command: a slice of a PCM bank copied into a chip's own RAM.

	The bank alone does not say where its bytes belong. `0x68` does - it names a position inside
	the bank, an address in the chip's memory, and a length - and the driver uses it to lay the
	samples out where its channels start. A song using these cannot be read by assuming the bank
	starts at address zero, which is what this branch assumed for a long time.
	*/
	struct RamCopy
	{
	uint8_t bankType = 0;	// which bank the bytes come from: 0x02 for the RF5C's
	uint32_t bankPosition = 0;
	uint32_t address = 0;	// in the chip's own memory
	uint32_t length = 0;
	uint32_t streamOrder = 0;	// position in the file, same counter as the writes
	};

/** A song reduced to "which chips, which register writes, which memory" - no format left in it.

	`writes` use `chips::RegisterWrite`, where `deviceId` is the index of the chip in `chips`.

	**The time-base contract (M-3)** - one rule, three places it shows up:

	1. a loader converts whatever its file counts in (VGM samples, CPU cycles, driver ticks) into
	   `tickRate` units and declares that rate here. A VGM loader says 44100 because its waits are
	   samples; an NSF loader would say 1.79 MHz because its writes are cycles. Nothing above the
	   loader needs to know which format produced the number;
	2. the assembly layer converts ticks to seconds exactly once, in `Builder::tickRate`, and hands
	   the same number to every decoder as `ChipConfig::timeBaseHz` - a decoder therefore never sees
	   an audio rate, only the unit its timestamps are in;
	3. the `MidiSequence` holds seconds, so the encoder, the UI and the pitch report are all
	   format-agnostic.

	This is the seam the review asked for (C-1): `midi/` reads this and produces a `MidiSequence`;
	`formats/*` writes this and knows nothing about MIDI. */
struct SongIr
{
	double tickRate = 44100.0;
	std::vector<ChipSlot> chips;
	std::vector<chips::RegisterWrite> writes;
	std::vector<RomBlock> roms;

	/** A `0xE0`: the driver moved the YM2612's PCM read pointer.

		The bank has no addresses of its own - it is one block of bytes - so this command is the only
		record of where a driver starts playing, and its distinct offsets are that driver's sample
		table. A chip that names ranges with registers does not need it; the YM2612 does.

		**What it cannot say**: which entry is a kick and which a snare (the file has offsets, not
		names), where an entry loops (the driver jumps back without a marker), or whether a rate was
		changed to transpose a drum (that only shows up as a different region rate). Those belong to
		the driver, and are not recoverable from the file - see the memo of 2026-11-10.
	*/
	struct DacSeek
	{
		uint32_t offset = 0;
		uint32_t sampleTime = 0;
	};

	/** The `0x68` copies, in file order. Empty on a song that lays its banks out by uploading the
		whole block at zero - and on every format that has no such command. */
	std::vector<RamCopy> ramCopies;

	/** The YM2612 PCM seeks, in stream order. */
	std::vector<DacSeek> dacSeeks;
	uint32_t totalTicks = 0;
};

/** A format's entry point: bytes in, IR out.

	One implementation per format (`VgmSongLoader` today, an NSF loader later). A loader may not know
	what a note is, and may not include a MIDI header. */
class ISongLoader
{
public:
	virtual ~ISongLoader() = default;

	/** @returns false when the image is not this format, or is damaged; `error` says why. */
	virtual bool load (const void* data, std::size_t size, SongIr& out, std::string& error) = 0;
};

} // namespace toybox::midi
