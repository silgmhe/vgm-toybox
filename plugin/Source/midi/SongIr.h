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
	/** The NES expansion chips. They are separate values rather than one "expansion" because each
		needs its own emulation, and because a module can use at most one of them. */
	nesVrc6,		// two pulses and a sawtooth
	nesN163,		// wavetable, up to eight voices
	nesVrc7,		// six FM voices
	nesFds,			// one wavetable voice with its own modulator
	nesMmc5,		// two pulses and PCM
	nesS5b,			// three squares, noise and envelope
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
	std::uint32_t clockHz = 0;
	std::uint32_t flags = 0;
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
	std::uint32_t romSize = 0;	// bytes of the ROM this block belongs to (ROM blocks only)
	std::uint32_t romOffset = 0;	// where in that ROM the payload belongs (ROM blocks only)
	std::uint32_t ramOffset = 0;	// where in the chip's own RAM (RAM blocks only)
	const std::uint8_t* data = nullptr;
	std::size_t size = 0;

	/** True when the block addresses the chip's RAM rather than a ROM image. */
	bool isRam = false;
};

/** Bytes the loader owns, for a format whose register stream needs sample data that the file does
	not carry at a usable offset - a compiled module, for instance, whose DPCM samples have to be
	laid out before they can be addressed.

	`roms` points into the file image and stays a view; this is the exception, and it is a vector so
	the loader can build it. A VGM leaves it empty. */
struct OwnedRom
{
	int memoryType = 0;				// 0x08 for the NES APU's sample area, per the VGM data block types
	std::vector<std::uint8_t> bytes;
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
	std::vector<OwnedRom> ownedRoms;
	std::uint32_t totalTicks = 0;
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
