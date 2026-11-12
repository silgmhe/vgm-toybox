#include "WonderswanSamples.h"

#include <cstdint>
#include <cstdio>
#include <map>

#include "SampleCollector.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	constexpr uint32_t kDefaultClock = 3072000;

	constexpr int kChannels = 4;
	constexpr int kTableBytes = 16;			// two 4-bit steps per byte
	constexpr int kWaveStride = 0x10;
	constexpr std::size_t kRamBytes = 0x4000;

	constexpr uint16_t kWaveOffsetMask = 0xFFF0;
	constexpr int kWaveBaseShift = 6;

	/** `0xFFFF` is the one frequency the core treats as silence on a tone channel. */
	constexpr uint16_t kSilentFrequency = 0xFFFF;

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
		uint8_t ram[kRamBytes] = {};
		uint16_t waveBase = 0;
		uint16_t frequency[kChannels] = {};
		bool keyed[kChannels] = {};
		uint8_t mode = 0;					// 0x90: enables, voice, sweep, noise
	};
}

std::vector<playback::DecodedSample> collectWonderswanSamples (const midi::SongIr& ir,
															   uint32_t clockHz)
{
	SampleCollector collector;

	uint32_t clock = clockHz;

	if (clock == 0)
	{
		for (const auto& slot : ir.chips)
		{
			if (slot.kind == midi::ChipKind::wonderswan && slot.clockHz != 0)
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

		// Channel 3 with the LFSR on, and channel 1 in voice mode, have no table to walk.
		if (channel == 3 && (chip.mode & 0x80) != 0)
		{
			return;
		}

		if (channel == 1 && (chip.mode & 0x20) != 0)
		{
			return;
		}

		const uint16_t frequency = chip.frequency[channel];

		if (frequency == kSilentFrequency)
		{
			return;
		}

		const std::size_t base = (std::size_t) ((chip.waveBase + channel * kWaveStride)
												 & kWaveOffsetMask);

		// **Expanded to one level per byte**, which is what `prom4` reads: two 4-bit steps per byte,
		// the **low** nibble first (`ws_audio.c:339-344`).
		uint8_t levels[32] = {};

		for (int step = 0; step < 32; ++step)
		{
			const uint8_t byte = chip.ram[(base + (std::size_t) (step / 2)) & (kRamBytes - 1)];
			levels[step] = (step % 2 == 0) ? (uint8_t) (byte & 0x0F) : (uint8_t) (byte >> 4);
		}

		const uint64_t key = hashOf (levels);

		char name[40] = {};
		std::snprintf (name, sizeof (name), "wonderswan_%08X%08X", (unsigned) (key >> 32),
					   (unsigned) (key & 0xFFFFFFFFull));

		const double rate = (double) clock / (double) (2048 - (frequency & 0x07FF));

		collector.add (key, name, levels, sizeof (levels), SampleFormat::prom4, rate, 0,
					   /*isCycle*/ true);
	};

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::wonderswan)
		{
			continue;
		}

		Chip& chip = chips[write.deviceId];

		// The tables arrive as RAM bytes, not as registers (`0xC6`).
		if (write.isMemory)
		{
			const std::size_t address = (((std::size_t) write.port << 8) | (std::size_t) write.reg);

			if (address < kRamBytes)
			{
				chip.ram[address] = (uint8_t) write.value;
			}

			continue;
		}

		const int reg = (int) write.reg;
		const int value = (int) (write.value & 0xFF);

		if (reg >= 0x80 && reg <= 0x87)
		{
			const int channel = (reg - 0x80) >> 1;
			const uint16_t mask = ((reg - 0x80) & 1) != 0 ? 0x0700 : 0x00FF;
			const int shift = ((reg - 0x80) & 1) != 0 ? 8 : 0;

			chip.frequency[channel] = (uint16_t) ((chip.frequency[channel] & (uint16_t) ~mask)
												  | ((uint16_t) value << shift));

			if (chip.keyed[channel])
			{
				emit (chip, channel);		// the same table at a new pitch is a new file
			}

			continue;
		}

		if (reg == 0x8F)
		{
			chip.waveBase = (uint16_t) ((value << kWaveBaseShift) & 0x3FFF);
			continue;
		}

		if (reg == 0x90)
		{
			chip.mode = (uint8_t) value;

			for (int channel = 0; channel < kChannels; ++channel)
			{
				const bool on = (value & (1 << channel)) != 0;

				if (on && ! chip.keyed[channel])
				{
					emit (chip, channel);
				}

				chip.keyed[channel] = on;
			}
		}
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
