#include "Ym2612DacSampler.h"

#include <algorithm>
#include <utility>

#include "midi/SongIr.h"

namespace toybox::backends::libvgm::dac
{
namespace
{
	/** One thing that happened to the DAC: a seek, or a byte the stream delivered. */
	struct Event
	{
		uint32_t time = 0;
		uint32_t order = 0;		// stream position, for two events at one instant
		uint32_t offset = 0;	// seek only
		bool seek = false;
	};

	bool wasEarlier (const Event& a, const Event& b)
	{
		if (a.time != b.time)
		{
			return a.time < b.time;
		}

		// A seek at the same instant comes first: the driver moves the pointer, then feeds it.
		if (a.seek != b.seek)
		{
			return a.seek;
		}

		return a.order < b.order;
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
	std::vector<Event> events;

	for (const auto& write : ir.writes)
	{
		if ((std::size_t) write.deviceId == device && write.port == 0 && write.reg == 0x2A
			&& write.fromStream)
		{
			Event event;
			event.time = write.sampleTime;
			event.order = write.streamOrder;
			events.push_back (event);
		}
	}

	for (const auto& seek : ir.dacSeeks)
	{
		Event event;
		event.time = seek.sampleTime;
		event.offset = seek.offset;
		event.seek = true;
		events.push_back (event);
	}

	if (events.empty ())
	{
		return {};
	}

	std::stable_sort (events.begin (), events.end (), wasEarlier);

	Sampler sampler (tickRate);

	for (const auto& event : events)
	{
		if (event.seek)
		{
			sampler.seek (event.time, event.offset);
		}
		else
		{
			sampler.byte (event.time);
		}
	}

	sampler.finish ();

	return sampler.runs ();
}

} // namespace toybox::backends::libvgm::dac
