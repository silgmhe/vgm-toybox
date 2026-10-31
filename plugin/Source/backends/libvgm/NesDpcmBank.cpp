#include "NesDpcmBank.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

#include "VgmCommandReader.h"
#include "chips/NesDpcmSample.h"

namespace toybox::backends::libvgm
{

/** The 2A03's arithmetic, which lives beside the chip decoders and is reachable from either
	front end. */
namespace dpcm = toybox::chips::dpcm;

namespace
{
	/** The window a 2A03 can read DPCM from, as the registers name it. Absolute, because `$4012`
		addresses it absolutely - keeping the image in those terms leaves the arithmetic in one
		place instead of at every use. */
	constexpr std::uint32_t kDpcmBase = 0xC000;
	constexpr std::uint32_t kDpcmSize = 0x4000;

	/** One start, as the three registers described it. */
	struct Start
	{
		std::uint16_t address = 0;
		std::uint16_t length = 0;
		std::uint8_t rateIndex = 0;
	};

	/** The 2A03's clock from the header, which says whether the rate table is the NTSC one.

		Read here rather than asked of VgmCommandReader: that class walks the stream, and the clock
		is in the header it was handed. 0x74 is where VGM 1.51 put it, and this chip's field has
		not moved since.
	*/
	std::uint32_t nesClockOf (const void* vgm, std::size_t size)
	{
		if (vgm == nullptr || size < 0x78)
		{
			return 0;
		}

		const auto* bytes = static_cast<const std::uint8_t*> (vgm);

		return ((std::uint32_t) bytes[0x74]
				| ((std::uint32_t) bytes[0x75] << 8)
				| ((std::uint32_t) bytes[0x76] << 16)
				| ((std::uint32_t) bytes[0x77] << 24)) & 0x3FFFFFFFu;
	}
}

std::vector<playback::DecodedSample> collectNesDpcmSamples (const void* vgm, std::size_t size)
{
	std::vector<playback::DecodedSample> out;

	if (vgm == nullptr || size == 0)
	{
		return out;
	}

	VgmCommandReader reader (vgm, size);

	std::vector<Start> starts;

	std::uint8_t pendingPage = 0;
	std::uint8_t pendingLength = 0;
	std::uint8_t pendingRate = 0;
	bool enabled = false;

	VgmCommand command;

	while (reader.next (command))
	{
		if (command.command != 0xB4)
		{
			continue;
		}

		// The address byte's bit 7 is the second-chip flag everywhere in VGM; on this chip there
		// is no second chip and the registers live below 0x80 anyway.
		const auto reg = (std::uint8_t) (command.addr & 0x7F);

		if (reg == 0x10)
		{
			pendingRate = command.data;
		}
		else if (reg == 0x12)
		{
			pendingPage = command.data;
		}
		else if (reg == 0x13)
		{
			pendingLength = command.data;
		}
		else if (reg == 0x15)
		{
			// The 0-to-1 edge of the enable bit, the same one the hardware starts on. A driver
			// that rewrites $4015 every frame would otherwise be read as starting a sample on
			// each write, and what it started would be whatever the registers happened to say.
			const bool now = (command.data & 0x10) != 0;

			if (now && ! enabled)
			{
				starts.push_back ({ (std::uint16_t) (kDpcmBase + (std::uint32_t) pendingPage * 64u),
									(std::uint16_t) ((std::uint32_t) pendingLength * 16u + 1u),
									pendingRate });
			}

			enabled = now;
		}
	}

	if (starts.empty ())
	{
		return out;
	}

	// The image, assembled from the data blocks. Read after the walk rather than during it: the
	// reader collects blocks as it steps, so by now the list is complete and in stream order - and
	// a later block for the same address overwrites an earlier one, which is what the writer of a
	// resumed recording meant.
	std::vector<std::uint8_t> image (kDpcmSize, 0);

	for (const auto& block : reader.dataBlocks ())
	{
		// RAM-write blocks carry the address in the block header; ROM blocks carry an offset into
		// a ROM the chip maps. Both end up at an address in this window, which is the only thing
		// the slicing below cares about.
		const bool ram = block.type >= 0xC0;
		const std::uint32_t address = ram ? block.ramOffset
										  : kDpcmBase + block.romOffset;

		if (block.data == nullptr || address < kDpcmBase
			|| address + block.size > kDpcmBase + kDpcmSize)
		{
			continue;
		}

		const auto offset = (std::size_t) (address - kDpcmBase);

		for (std::size_t i = 0; i < block.size; ++i)
		{
			image[offset + i] = block.data[i];
		}
	}

	// One entry per distinct sample, in the order the song first started it.
	std::vector<Start> unique;

	for (const auto& start : starts)
	{
		bool known = false;

		for (const auto& seen : unique)
		{
			known = known || (seen.address == start.address && seen.length == start.length);
		}

		if (! known)
		{
			unique.push_back (start);
		}
	}

	const auto clock = nesClockOf (vgm, size);

	// PAL's 2A03 runs at 1.66 MHz; anything in the NTSC range is treated as NTSC, which is what
	// both of this project's other NES paths do.
	const bool pal = clock > 0 && clock < 1700000u;

	for (const auto& sample : unique)
	{
		const auto offset = (std::uint32_t) sample.address - kDpcmBase;
		const auto available = kDpcmSize - offset;
		const auto length = (std::size_t) (std::min) ((std::uint32_t) sample.length, available);

		if (length < 2)
		{
			// One byte is the padding byte and nothing else - a start that names no data.
			continue;
		}

		const std::vector<std::uint8_t> bytes (image.begin () + offset, image.begin () + offset + length);
		const auto trimmed = dpcm::finalLength (bytes.size (), bytes.data ());

		if (trimmed < 2)
		{
			continue;
		}

		char address[8] = {};
		std::snprintf (address, sizeof (address), "%04X", (unsigned) sample.address);

		playback::DecodedSample entry;
		entry.name = std::string ("dpcm_") + address + "_" + std::to_string ((unsigned long) trimmed);
		entry.values = dpcm::decode (bytes.data (), trimmed);
		entry.rate = dpcm::rate (sample.rateIndex, pal);

		out.push_back (std::move (entry));
	}

	return out;
}

} // namespace toybox::backends::libvgm
