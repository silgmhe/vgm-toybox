#include "VsuSamples.h"

#include <cstdint>
#include <cstdio>
#include <map>

#include "SampleCollector.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	constexpr uint32_t kDefaultClock = 5370000;

	/** `vsu.c:116` - five tables of thirty-two six-bit entries, and the sixth voice is noise. */
	constexpr int kTables = 5;
	constexpr int kTableBytes = 32;
	constexpr int kNoiseChannel = 5;

	/** `2048 - freq` is the divider, so the range is 1..2048. */
	constexpr double kDividerRange = 2048.0;

	/** How many entries a voice walks per second, which is the rate a one-cycle file declares. */
	double stepRateOf (uint32_t clock, uint16_t freq)
	{
		const double remaining = kDividerRange - (double) (freq & 0x7FF);

		if (remaining <= 0.0)
		{
			return 0.0;
		}

		return (double) clock / remaining;
	}

	uint64_t hashOf (const uint8_t* bytes)
	{
		uint64_t hash = 1469598103934665603ull;

		for (int i = 0; i < kTableBytes; ++i)
		{
			hash ^= (uint64_t) bytes[i];
			hash *= 1099511628211ull;
		}

		return hash;
	}

	struct Chip
	{
		uint8_t waveRam[kTables][kTableBytes] = {};
		int ramAddress[6] = { 0, 0, 0, 0, 0, 0 };
		uint16_t freq[6] = { 0, 0, 0, 0, 0, 0 };
		bool keyed[6] = { false, false, false, false, false, false };
	};

	struct Pending
	{
		std::vector<uint8_t> bytes;
		double rate = 0.0;
	};
}

std::vector<playback::DecodedSample> collectVsuSamples (const midi::SongIr& ir, uint32_t clockHz)
{
	SampleCollector collector;

	uint32_t clock = clockHz;

	if (clock == 0)
	{
		for (const auto& slot : ir.chips)
		{
			if (slot.kind == midi::ChipKind::vsu && slot.clockHz != 0)
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
	std::map<uint64_t, Pending> pending;

	/** Files the table a voice points at. Identity is the table's own contents: the same thirty-two
		bytes at two pitches are one wave, and two sections that rewrite a table are two. */
	const auto emit = [&] (Chip& chip, int channel)
	{
		if (channel == kNoiseChannel || channel < 0 || channel >= 6)
		{
			return;
		}

		const int table = chip.ramAddress[channel];

		if (table < 0 || table >= kTables || chip.freq[channel] == 0)
		{
			return;
		}

		const double rate = stepRateOf (clock, chip.freq[channel]);

		if (rate <= 0.0)
		{
			return;
		}

		const uint64_t key = hashOf (chip.waveRam[table]);
		Pending& entry = pending[key];

		if (entry.bytes.empty())
		{
			entry.bytes.assign (chip.waveRam[table], chip.waveRam[table] + kTableBytes);
		}

		// Last write wins, as everywhere else: a table played at several pitches gets one file at the
		// most recent rate, which is also the one the row shows last.
		entry.rate = rate;
	};

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::vsu)
		{
			continue;
		}

		Chip& chip = chips[write.deviceId];

		// The low half of the address space: the wave RAM, and the modulation table above it.
		if (write.port == 0)
		{
			if (write.reg < (kTables * kTableBytes))
			{
				chip.waveRam[write.reg / kTableBytes][write.reg % kTableBytes] =
					(uint8_t) (write.value & 0x3F);
			}

			continue;
		}

		const int channel = (write.reg >> 4) & 0x0F;

		if (channel > 5)
		{
			continue;
		}

		switch (write.reg & 0x0F)
		{
		case 0x0:		// IntlControl: bit 7 keys the voice
		{
			const bool wasKeyed = chip.keyed[channel];
			chip.keyed[channel] = (write.value & 0x80) != 0;

			if (chip.keyed[channel] && ! wasKeyed)
			{
				emit (chip, channel);
			}

			break;
		}

		case 0x2:		// frequency, low
			chip.freq[channel] = (uint16_t) ((chip.freq[channel] & 0x700) | (write.value & 0xFF));

			if (chip.keyed[channel])
			{
				emit (chip, channel);
			}

			break;

		case 0x3:		// frequency, high three bits
			chip.freq[channel] = (uint16_t) ((chip.freq[channel] & 0x0FF) | ((write.value & 0x07) << 8));

			if (chip.keyed[channel])
			{
				emit (chip, channel);
			}

			break;

		case 0x6:		// **the wave table index** (`vsu.c:358`), not 0x5 - that one is EnvControl's
			chip.ramAddress[channel] = (int) (write.value & 0x0F);

			if (chip.keyed[channel])
			{
				emit (chip, channel);
			}

			break;

		default:		// 0x1 sweep, 0x4/0x5 envelope, 0x7 sweep control, 0x8..0xF levels
			break;
		}
	}

	for (auto& [key, entry] : pending)
	{
		if (entry.bytes.size () != kTableBytes || entry.rate <= 0.0)
		{
			continue;
		}

		char name[32] = {};
		std::snprintf (name, sizeof (name), "vsu_%08X%08X", (unsigned) (key >> 32),
					   (unsigned) (key & 0xFFFFFFFFull));

		collector.add (key, name, entry.bytes.data (), entry.bytes.size (), SampleFormat::vsu6,
					   entry.rate, 0, /*isCycle*/ true);
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
