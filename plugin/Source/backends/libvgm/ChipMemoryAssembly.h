#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "chips/ChipTypes.h"
#include "chips/IChipDecoder.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm::memory
{

/** Builds a chip's memory image from the song's data blocks.

	Three shapes, told apart by the block type the file uses: a block below 0x40 is a PCM bank whose
	blocks concatenate in stream order, a RAM block names an address to be written at, and a ROM
	block declares the size of the whole image and where its payload belongs inside it.

	Here rather than in either caller because two of them need it and they must agree: the MIDI
	builder, which decodes a whole song to extract notes, and the display's replay, which feeds the
	same decoders while the transport moves. A sampled chip whose ROM is missing does not fail - it
	measures the wrong pitch, or no pitch, and the row shows a note that belongs to nothing. That is
	the C352 reporting C9 with an empty image.
*/

/** The C140's ROM banking travels in the chip slot's flags, the way the header byte did before. */
uint8_t c140Banking (const midi::SongIr& ir);

std::vector<uint8_t> rom (const midi::SongIr& ir, uint8_t type, std::size_t maxBytes,
						  bool clipToContent);

/** A `0x67` block of type 0x00-0x3F: bytes concatenate in stream order, no header and no address. */
std::vector<uint8_t> pcmBank (const midi::SongIr& ir, uint8_t type, std::size_t maxBytes);

/** A chip's RAM image: `maxBytes` of zeroes with every RAM block copied to its own address. */
std::vector<uint8_t> ram (const midi::SongIr& ir, uint8_t type, std::size_t maxBytes);

/** Whatever the decoder asked for, built the way its `kRequirements` describes.

	Empty when it asked for nothing, or when the song carries no block of that type - which is not an
	error: a file that never uploads its ROM leaves every decoder reading zeroes, and a wavetable
	that is all zeroes measures as "no period". `banking` is filled for the C140 and left alone
	otherwise.
*/
std::vector<uint8_t> forRequirements (const midi::SongIr& ir,
									  const chips::ChipRequirements& requirements,
									  uint8_t& banking);

} // namespace toybox::backends::libvgm::memory
