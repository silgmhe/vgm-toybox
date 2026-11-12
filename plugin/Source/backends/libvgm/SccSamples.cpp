#include "SccSamples.h"

#include <cstdint>
#include <cstdio>
#include <map>

#include "SampleCollector.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	constexpr uint32_t kDefaultClock = 3579545;

	/** Five voices of thirty-two signed bytes; SCC mode shares one table between 3 and 4. */
	constexpr int kChannels = 5;
	constexpr int kTableBytes = 32;

	/** Addresses at or above this are channel 3/4's shared half in SCC mode (`k051649.c:268`). */
	constexpr int kSharedTableStart = 0x60;

	uint64_t hashOf (const uint8_t* bytes)
	{
		uint64_t hash = 1469598103934665603ull;

		for (int i = 0; i < kTableBytes; ++i)
		{
			hash = (hash ^ (uint64_t) bytes[i]) * 1099511628211ull;
		}

		return hash;
	}

	struct Chip
	{
		uint8_t wave[kChannels][kTableBytes] = {};
		uint16_t frequency[kChannels] = {};
		bool keyed[kChannels] = {};
		uint8_t latch = 0;				// the register number the next data byte goes to
	};
}

std::vector<playback::DecodedSample> collectSccSamples (const midi::SongIr& ir, uint32_t clockHz,
														bool plusMode)
{
	SampleCollector collector;

	uint32_t clock = clockHz;

	if (clock == 0)
	{
		for (const auto& slot : ir.chips)
		{
			if (slot.kind == midi::ChipKind::k051649 && slot.clockHz != 0)
			{
				clock = slot.clockHz;
			}
		}
	}

	if (clock == 0)
	{
		clock = kDefaultClock;
	}

	std::map<uint32_t, Chip> chips;

	const auto emit = [&] (Chip& chip, int channel)
	{
		if (channel < 0 || channel >= kChannels)
		{
			return;
		}

		// `2 * clock / (f + 1)` is the rate the counter steps the table at; the denominator is never
		// zero, so unlike the HuC6280 there is no substitution to make here.
		const double rate = 2.0 * (double) clock / ((double) (chip.frequency[channel] & 0x0FFF) + 1.0);

		if (rate <= 0.0)
		{
			return;
		}

		const uint64_t key = hashOf (chip.wave[channel]);

		char name[32] = {};
		std::snprintf (name, sizeof (name), "scc_%08X%08X", (unsigned) (key >> 32),
					   (unsigned) (key & 0xFFFFFFFFull));

		collector.add (key, name, chip.wave[channel], kTableBytes, SampleFormat::pcm8Signed, rate, 0,
					   /*isCycle*/ true);
	};

	/** One wave-RAM byte, placed the way the chip places it: SCC+ by the top bits of the address,
		SCC by the shared boundary at 0x60. */
	const auto writeWave = [&] (Chip& chip, uint8_t address, uint8_t value)
	{
		const uint8_t step = (uint8_t) (address & (kTableBytes - 1));

		if (plusMode)
		{
			const int channel = address >> 5;

			if (channel < kChannels)
			{
				chip.wave[channel][step] = value;
			}
		}
		else if (address >= kSharedTableStart)
		{
			chip.wave[3][step] = value;
			chip.wave[4][step] = value;
		}
		else
		{
			const int channel = address >> 5;

			if (channel < kChannels)
			{
				chip.wave[channel][step] = value;
			}
		}
	};

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::k051649)
		{
			continue;
		}

		Chip& chip = chips[write.deviceId];

		const int block = (int) (write.reg >> 1);
		const bool isData = (write.reg & 1) != 0;
		const uint8_t value = (uint8_t) (write.value & 0xFF);

		if (! isData)
		{
			chip.latch = value;			// the register number the next byte belongs to
			continue;
		}

		const uint8_t address = chip.latch;

		switch (block)
		{
		case 0:						// wave RAM, low half
		case 4:						// and the half SCC+ gives channel 4
			writeWave (chip, address, value);
			break;

		case 1:						// frequency: 0x00/0x01 channel 0, ..., 0x08/0x09 channel 4
		{
			const int channel = address >> 1;

			if (channel >= kChannels)
			{
				break;
			}

			const uint16_t mask = (address & 1) != 0 ? 0x0F00 : 0x00FF;
			const int shift = (address & 1) != 0 ? 8 : 0;

			chip.frequency[channel] = (uint16_t) ((chip.frequency[channel] & (uint16_t) ~mask)
												  | ((uint16_t) value << shift));

			if (chip.keyed[channel])
			{
				emit (chip, channel);	// the same table at a new pitch is a new file
			}

			break;
		}

		case 3:						// key mask, one bit per channel
			for (int channel = 0; channel < kChannels; ++channel)
			{
				const bool on = (value & (1u << channel)) != 0;

				if (on && ! chip.keyed[channel])
				{
					emit (chip, channel);
				}

				chip.keyed[channel] = on;
			}

			break;

		default:					// 2 volume, 5 test/deform
			break;
		}
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
