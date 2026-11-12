#include "GameBoySamples.h"

#include <cstdint>
#include <cstdio>
#include <map>

#include "SampleCollector.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	constexpr uint32_t kDefaultClock = 4194304;

	constexpr int kTableBytes = 16;
	constexpr int kWaveRamFirst = 0x20;		// 0xB3 offsets: 0xFF30 - 0xFF10
	constexpr int kWaveRamLast = 0x2F;

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
		uint8_t ram[kTableBytes] = {};
		uint16_t frequency = 0;
		bool powered = false;			// NR52 bit 7
		bool dacEnabled = false;		// NR30 bit 7
	};
}

std::vector<playback::DecodedSample> collectGameBoySamples (const midi::SongIr& ir, uint32_t clockHz)
{
	SampleCollector collector;

	uint32_t clock = clockHz;

	if (clock == 0)
	{
		for (const auto& slot : ir.chips)
		{
			if (slot.kind == midi::ChipKind::gameBoy && slot.clockHz != 0)
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

	const auto emit = [&] (Chip& chip)
	{
		if (! chip.powered || ! chip.dacEnabled)
		{
			return;
		}

		// **Expanded to one level per byte**, the **high** nibble first (`gb_mame.c:990-997`).
		uint8_t levels[32] = {};

		for (int step = 0; step < 32; ++step)
		{
			const uint8_t byte = chip.ram[step / 2];
			levels[step] = (step % 2 == 0) ? (uint8_t) (byte >> 4) : (uint8_t) (byte & 0x0F);
		}

		const uint64_t key = hashOf (levels);

		char name[32] = {};
		std::snprintf (name, sizeof (name), "gb_%08X%08X", (unsigned) (key >> 32),
					   (unsigned) (key & 0xFFFFFFFFull));

		// The counter ticks every two cycles and the offset advances once per reload.
		const double rate = (double) clock
							/ (2.0 * (2048.0 - (double) (chip.frequency & 0x07FF)));

		collector.add (key, name, levels, sizeof (levels), SampleFormat::prom4, rate, 0,
					   /*isCycle*/ true);
	};

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::gameBoy)
		{
			continue;
		}

		Chip& chip = chips[write.deviceId];

		const int reg = (int) write.reg;
		const int value = (int) (write.value & 0xFF);

		if (reg >= kWaveRamFirst && reg <= kWaveRamLast)
		{
			chip.ram[reg - kWaveRamFirst] = (uint8_t) value;
			continue;
		}

		switch (reg)
		{
		case 0x0A:					// NR30: the wave DAC
			chip.dacEnabled = (value & 0x80) != 0;
			break;

		case 0x0D:					// frequency, low
			chip.frequency = (uint16_t) ((chip.frequency & 0x0700) | value);
			break;

		case 0x0E:					// frequency high three bits and the trigger
			chip.frequency = (uint16_t) ((chip.frequency & 0x00FF) | ((value & 0x07) << 8));

			// The trigger is a write, not a state: only a byte with bit 7 set starts a note.
			if ((value & 0x80) != 0)
			{
				emit (chip);
			}

			break;

		case 0x16:					// NR52: the APU's power switch
			chip.powered = (value & 0x80) != 0;
			break;

		default:					// NR31 length, NR32 volume, NR34's other bits
			break;
		}
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
