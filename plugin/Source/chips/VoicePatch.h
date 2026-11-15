#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace toybox::chips
{

/** Which chip a voice belongs to.

	**Its own enum, not libvgm's `DEVID_*`.** Those name a device *instance* inside one backend, and
	a chip that arrives from an NSF file has no such handle at all. This is the model - "what is
	this sound made by" - which both backends can answer, and which a saved patch can carry.
*/
enum class ChipModel : uint8_t
{
	unknown = 0,
	ym2612,		// OPN2, six FM channels and a DAC
	ym2608,		// OPNA, the PC-98 part
	ym2610,		// OPNB, the Neo Geo part
	ym2151,		// OPM, eight four-operator channels with a different envelope layout
	ym3812,		// OPL2, two operators, eight registers per voice
	vrc7		// the NES expansion chip, an OPL2 relative
};

/** **The registers behind one channel's voice**, as the chip itself addresses them.

	**Offsets are 16 bits because the chip has two ports.** The OPN's second port carries channels
	3-5, and its registers are written with the port in bit 8. An 8-bit offset would make channel 4
	indistinguishable from channel 0 - the low two bits of both are zero - which is a patch that
	loads somewhere else.

	`(offset, value)` pairs rather than a struct of named fields, and that is deliberate:

	- it is what a replayer needs: `load()` writes them back in order, with no encoding step in
	  between;
	- it assumes no layout - the YM2612's patch is 31 bytes over four operators, the VRC7's is 8
	  over two, and one array holds either;
	- a struct of named fields would have to be turned *back* into register writes before anything
	  could be played, and that encoder would be a second authority over the same numbers. That is
	  the shape this project has already paid for twice (`tableWave`, the SSG's channel count).

	The offsets are the chip's own (0x30..0xB0 for the OPN family), so an empty `regs` and a
	`model` of `unknown` are the two ways of saying "no patch". A decoder that cannot serialise
	its voice says so by returning false from `capturePatch()` rather than inventing one.
*/
/** **The rate a computed note is sampled at.**

	Low on purpose: the window draws it at seventy pixels and the exporter writes it as a file, and
	neither wants 44 100 points for a note whose whole point is its envelope. It lives here rather
	than beside the renderer because the *exporter* needs it too, and `playback/` may not include an
	engine header - the build enforces that.

	It has to be shared: a file that declares 44 100 over 8 000-rate samples plays four octaves high
	and five times too short, which is exactly what the first export of a note sounded like.
*/
constexpr double kNoteRate = 8000.0;

/** The two stages of a rendered note, in seconds. Both are settings; these are the fallbacks.

	They are separate because they answer different questions. The sustain is how long the sound is
	held before the key comes up - a choice, since the chip's "S" is a level and it decays through
	it rather than holding. The release is the tail after the key, which the patch does define, but
	its rate code reaches four minutes at RR = 1, so the length is capped here rather than read.
*/
constexpr double kDefaultSustainSeconds = 2.0;
constexpr double kDefaultReleaseSeconds = 1.0;

/** **How many bits of magnitude a chip's FM core actually produces.**

	Not the DAC's width. An eight-bit converter fed by a fourteen-bit sum still leaves all fourteen
	bits in `DEV_SMPL`, and quantising to the converter discards detail the emulator produces - a
	different sound, not a more authentic one.

	The figures follow each core's own accumulation rather than a datasheet: `fmopn` sums four
	operators of about `+/-0x1000` into `out_fm`, which is fourteen bits; the OPL and OPM cores
	accumulate wider and use more of `DEV_SMPL`'s sixteen. Adding a chip means adding a line here
	next to the model, which is why the table sits beside the enum instead of inside a renderer.
*/
constexpr int fmOutputBits (ChipModel model)
{
	switch (model)
	{
	case ChipModel::ym2612:
	case ChipModel::ym2608:
	case ChipModel::ym2610:		return 14;		// fmopn, one core for the whole OPN family

	case ChipModel::ym2151:						// fmopm
	case ChipModel::ym3812:						// fmopl
	case ChipModel::vrc7:		return 16;

	case ChipModel::unknown:	break;
	}

	return 16;
}

struct VoicePatch
{
	ChipModel model = ChipModel::unknown;
	std::vector<std::pair<uint16_t, uint16_t>> regs;
};

} // namespace toybox::chips
