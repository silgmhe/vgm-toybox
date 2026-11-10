#include "Ym2612DacSampler.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "midi/SongIr.h"

namespace toybox::backends::libvgm::dac
{
namespace
{
	bool isStreamByte (const chips::RegisterWrite& write, std::size_t device)
	{
		return (std::size_t) write.deviceId == device && write.port == 0 && write.reg == 0x2A
			   && write.fromStream;
	}
}

std::vector<uint8_t> bankFor (const midi::SongIr& ir, std::size_t maxBytes)
{
	std::vector<uint8_t> bank;

	for (const auto& block : ir.roms)
	{
		if (bank.size () >= maxBytes)
		{
			break;
		}

		// Type 0x00 is the plain PCM block; 0x80 is the same bytes tagged as ROM, and the reader has
		// already spent its eight-byte size/offset header.
		if (block.isRam || block.data == nullptr
			|| (block.memoryType != 0x00 && block.memoryType != 0x80))
		{
			continue;
		}

		const std::size_t count = (std::min) (block.size, maxBytes - bank.size ());
		bank.insert (bank.end (), block.data, block.data + count);
	}

	return bank;
}

std::vector<Run> runsFor (const midi::SongIr& ir, std::size_t device, uint32_t tickRate)
{
	Sampler sampler (tickRate);

	// **Two inputs, one timeline, one pass.** The `0x8n` writes and the `0xE0` seeks are both already
	// in stream order, so merging them needs no event list and no sort. Building that list and
	// sorting it cost 16 ms per call on `02 Green Hill Zone` (459 546 writes, a 7 MB temporary), and
	// it was paid once per track player.
	const auto& writes = ir.writes;
	const auto& seeks = ir.dacSeeks;

	std::size_t w = 0;
	std::size_t s = 0;

	for (;;)
	{
		while (w < writes.size () && ! isStreamByte (writes[w], device))
		{
			++w;
		}

		const bool haveByte = w < writes.size ();
		const bool haveSeek = s < seeks.size ();

		if (! haveByte && ! haveSeek)
		{
			break;
		}

		const auto byteTime = haveByte ? writes[w].sampleTime : std::numeric_limits<uint32_t>::max ();
		const auto seekTime = haveSeek ? seeks[s].sampleTime : std::numeric_limits<uint32_t>::max ();

		// A seek at the same instant comes first: the driver moves the pointer, then feeds it. That is
		// the order the sort used to produce, and the runs - and the exported names - depend on it.
		if (haveSeek && seekTime <= byteTime)
		{
			sampler.seek (seekTime, seeks[s].offset);
			++s;
		}
		else
		{
			sampler.byte (byteTime);
			++w;
		}
	}

	sampler.finish ();

	return sampler.runs ();
}

std::shared_ptr<const Table> buildTable (const midi::SongIr& ir, std::size_t maxBankBytes)
{
	auto table = std::make_shared<Table> ();
	table->tickRate = (uint32_t) std::llround (ir.tickRate > 0.0 ? ir.tickRate : 44100.0);
	table->bank = bankFor (ir, maxBankBytes);

	for (std::size_t device = 0; device < ir.chips.size (); ++device)
	{
		if (ir.chips[device].kind == midi::ChipKind::ym2612)
		{
			table->device = device;
			table->runs = runsFor (ir, device, table->tickRate);
			break;			// one YM2612: `0xE0` names no chip, and the loader creates one
		}
	}

	assignSampleIndices (table->runs, table->bank.size ());

	return table;
}

} // namespace toybox::backends::libvgm::dac
