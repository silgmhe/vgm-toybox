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
	ay8910,			// AY-3-8910 / YM2149, three squares plus noise and envelope
	c140,			// C140 and the 219 ASIC, sixteen sample voices
	c352,			// C352, 32 sample voices with word-wide registers
	es5503,			// ES5503, 32 oscillators walking wavetables
	ga20,			// GA20 / Y8950's ADPCM half
	gameBoy,		// DMG APU: two pulses, a wave channel and noise
	huc6280,		// PC Engine: six wavetable channels
	k005289,		// Konami: two wavetable voices
	k007232,		// Konami: two PCM voices
	k051649,		// SCC: five wavetable channels
	k053260,		// Konami PCM with KADPCM
	k054539,		// Konami 8-voice PCM
	mikey,			// Atari Lynx: four timer-driven squares
	msm5232,		// OKI: eight voices, the register is a key index
	multiPcm,		// YMW258 / MultiPCM, 28 voices behind a three-port interface
	nesApu,			// 2A03: two pulses, triangle, noise and DMC
	okim6295,		// OKI ADPCM, four voices at one DAC rate
	opl,			// YM2413, YM3812, YMF262 and Y8950's FM half
	opm,			// YM2151
	opn,			// YM2612, YM2203, YM2608, YM2610 - FM plus their SSG/ADPCM channels
	pokey,			// Atari POKEY
	qsound,			// Capcom QSound, 16 voices
	rf5c68,			// RF5C68 / RF5C164
	saa1099,		// Philips SAA1099
	segapcm,		// Sega PCM, 16 voices
	sn76489,		// SN76489 / SN76496
	vsu,			// Virtual Boy, five wavetables plus noise
	wonderswan,		// WonderSwan, four wavetable channels
	x1_010,			// Seta X1-010, 16 wavetable channels
	ymf271,			// Yamaha OPX
	ymz280b,		// Yamaha YMZ280B
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

	`address` is the offset inside the target memory and `data` the payload. Decoders that measure a
	sample's own cycle to place the octave take it from here, which is why this belongs to the IR
	rather than to one format's reader. */
struct RomBlock
{
	int memoryType = 0;			// the loader's numbering; each chip interprets its own
	std::uint32_t address = 0;
	std::vector<std::uint8_t> data;
};

/** A song reduced to "which chips, which register writes, which memory" - no format left in it.

	`writes` use `chips::RegisterWrite`, where `deviceId` is the index of the chip in `chips`. Times
	are in `tickRate` units, so a loader that works in 1.79 MHz CPU cycles says so and the assembly
	layer converts once.

	This is the seam the review asked for (C-1): `midi/` reads this and produces a `MidiSequence`;
	`formats/*` writes this and knows nothing about MIDI. */
struct SongIr
{
	double tickRate = 44100.0;
	std::vector<ChipSlot> chips;
	std::vector<chips::RegisterWrite> writes;
	std::vector<RomBlock> roms;
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
