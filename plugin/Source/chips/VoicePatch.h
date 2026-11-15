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
struct VoicePatch
{
	ChipModel model = ChipModel::unknown;
	std::vector<std::pair<uint8_t, uint8_t>> regs;
};

} // namespace toybox::chips
