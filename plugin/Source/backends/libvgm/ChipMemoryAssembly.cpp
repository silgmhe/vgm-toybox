#include "ChipMemoryAssembly.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace toybox::backends::libvgm::memory
{

uint8_t c140Banking (const midi::SongIr& ir)
{
	for (const auto& slot : ir.chips)
	{
		if (slot.kind == midi::ChipKind::c140)
		{
			return (uint8_t) slot.flags;
		}
	}

	return 0;
}

std::vector<uint8_t> rom (const midi::SongIr& ir, uint8_t type, std::size_t maxBytes,
						  bool clipToContent)
{
	// The declared size is what the chip can address; the content is what a mask is built from. They
	// differ on the Neo Geo: the header says 8 MB but only 2.8 MB of blocks are present, so libvgm
	// masks the ADPCM-B address with 4 MB and the driver's 0x6B9000 is really 0x2B9000, inside its
	// block. Clipping to the content reproduces that.
	std::size_t romSize = 0;

	for (const auto& block : ir.roms)
	{
		if (block.memoryType != type)
		{
			continue;
		}

		const std::size_t end = (std::size_t) block.romOffset + block.size;
		romSize = std::max (romSize, clipToContent ? end : (std::size_t) block.romSize);
	}

	std::vector<uint8_t> image;
	romSize = std::min (romSize, maxBytes);

	if (romSize == 0)
	{
		return image;
	}

	image.assign (romSize, 0);

	for (const auto& block : ir.roms)
	{
		if (block.memoryType != type || block.romOffset >= image.size())
		{
			continue;
		}

		const std::size_t count = std::min (block.size, image.size() - block.romOffset);
		std::memcpy (image.data() + block.romOffset, block.data, count);
	}

	return image;
}

std::vector<uint8_t> pcmBank (const midi::SongIr& ir, uint8_t type, std::size_t maxBytes)
{
	std::vector<uint8_t> bank;

	for (const auto& block : ir.roms)
	{
		if (block.memoryType != type || bank.size() >= maxBytes)
		{
			continue;
		}

		// A bank larger than the chip's RAM is cut at the chip's limit, because everything past it
		// is unreachable anyway.
		const std::size_t count = std::min (block.size, maxBytes - bank.size());
		bank.insert (bank.end(), block.data, block.data + count);
	}

	// **A bank only has addresses once the `0x68` copies are applied.** The command names a slice of
	// the bank, an address in the chip and a length, and the driver uses it to put each sample where
	// its channels start. Assuming the bank begins at address zero - which is what this function did
	// - gives an image the chip never had: on `01 - Palmtree Panic Zone Past` the emulator's memory
	// at 0x8000 reads `80 80 80 80` where the bank's first kilobytes hold `99 19 83 97`.
	std::vector<uint32_t> inRange;

	for (const auto& copy : ir.ramCopies)
	{
		if (copy.bankType == type)
		{
			inRange.push_back (copy.length);
		}
	}

	if (inRange.empty())
	{
		return bank;
	}

	// Sign-magnitude zero is 0x80, and it is what the chip reads as silence where nothing was
	// copied. Zero would be a full-scale negative excursion.
	std::vector<uint8_t> image (maxBytes, 0x80);

	for (const auto& copy : ir.ramCopies)
	{
		if (copy.bankType != type || copy.address >= image.size())
		{
			continue;
		}

		if (copy.bankPosition >= bank.size())
		{
			continue;
		}

		const std::size_t count = std::min ({ (std::size_t) copy.length,
										   bank.size() - copy.bankPosition,
										   image.size() - copy.address });

		std::memcpy (image.data() + copy.address, bank.data() + copy.bankPosition, count);
	}

	return image;
}

std::vector<uint8_t> ram (const midi::SongIr& ir, uint8_t type, std::size_t maxBytes)
{
	// Unlike a ROM block, a RAM block carries no size for the whole memory - it names the address to
	// write at and nothing else. A gap stays zero, which is also what the chip reads there, and a
	// table that lives in one measures as "no period".
	std::vector<uint8_t> image (maxBytes, 0);

	for (const auto& block : ir.roms)
	{
		if (block.memoryType != type || block.ramOffset >= image.size())
		{
			continue;
		}

		const std::size_t count = std::min (block.size, image.size() - block.ramOffset);
		std::memcpy (image.data() + block.ramOffset, block.data, count);
	}

	return image;
}

std::vector<uint8_t> forRequirements (const midi::SongIr& ir,
									  const chips::ChipRequirements& requirements,
									  uint8_t& banking)
{
	if (! requirements.hasMemory)
	{
		return {};
	}

	banking = c140Banking (ir);

	if (requirements.memoryType < 0x40)
	{
		return pcmBank (ir, requirements.memoryType, requirements.memoryBytes);
	}

	if (requirements.memoryIsRam)
	{
		return ram (ir, requirements.memoryType, requirements.memoryBytes);
	}

	return rom (ir, requirements.memoryType, requirements.memoryBytes, requirements.clipToContent);
}

} // namespace toybox::backends::libvgm::memory
