#include "HuC6280Samples.h"

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

	/** Six voices, thirty-two entries of five bits each (`c6280_mame.c:96`). */
	constexpr int kChannels = 6;
	constexpr int kTableBytes = 32;

	/** The frequency register is twelve bits, and a zero divider clocks at 4096 (the decoder's own
		substitution, `HuC6280Decoder::channelState`). */
	constexpr int kFrequencyBits = 0x0FFF;
	constexpr double kZeroDivider = 4096.0;

	/** A write at or above this offset is the wave selector or DDA, not a channel register. */
	constexpr int kSelectRegister = 0x00;
	constexpr int kFrequencyLow = 0x02;
	constexpr int kFrequencyHigh = 0x03;
	constexpr int kControl = 0x04;
	constexpr int kWaveRegister = 0x06;

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
		int index[kChannels] = {};
		uint8_t control[kChannels] = {};		// 0x04: bit 7 enable, bit 6 DDA
		uint16_t frequency[kChannels] = {};
		bool keyed[kChannels] = {};
		int selected = 0;						// 0x00, three bits
	};
}

std::vector<playback::DecodedSample> collectHuC6280Samples (const midi::SongIr& ir, uint32_t clockHz)
{
	SampleCollector collector;

	uint32_t clock = clockHz;

	if (clock == 0)
	{
		for (const auto& slot : ir.chips)
		{
			if (slot.kind == midi::ChipKind::huc6280 && slot.clockHz != 0)
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

	/** Files the table a voice is playing. Identity is the table's own contents: the same thirty-two
		bytes at two pitches are one wave, and two sections that rewrite a table are two.
	*/
	const auto emit = [&] (Chip& chip, int channel)
	{
		if (channel < 0 || channel >= kChannels)
		{
			return;
		}

		const int divider = chip.frequency[channel] & kFrequencyBits;
		const double rate = (double) clock / ((divider == 0) ? kZeroDivider : (double) divider);

		if (rate <= 0.0)
		{
			return;
		}

		const uint64_t key = hashOf (chip.wave[channel]);

		char name[32] = {};
		std::snprintf (name, sizeof (name), "huc6280_%08X%08X", (unsigned) (key >> 32),
					   (unsigned) (key & 0xFFFFFFFFull));

		collector.add (key, name, chip.wave[channel], kTableBytes, SampleFormat::huc5, rate, 0,
					   /*isCycle*/ true);
	};

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::huc6280)
		{
			continue;
		}

		Chip& chip = chips[write.deviceId];

		const int reg = (int) write.reg;
		const int value = (int) (write.value & 0xFF);

		switch (reg)
		{
		case kSelectRegister:
			chip.selected = value & 0x07;
			break;

		case kFrequencyLow:
			if (chip.selected >= kChannels)
			{
				break;
			}

			chip.frequency[chip.selected] = (uint16_t) ((chip.frequency[chip.selected] & 0x0F00)
														| (value & 0xFF));

			if (chip.keyed[chip.selected])
			{
				emit (chip, chip.selected);		// the same table at a new pitch is a new file
			}

			break;

		case kFrequencyHigh:
			if (chip.selected >= kChannels)
			{
				break;
			}

			chip.frequency[chip.selected] = (uint16_t) ((chip.frequency[chip.selected] & 0x00FF)
														| ((value & 0x0F) << 8));

			if (chip.keyed[chip.selected])
			{
				emit (chip, chip.selected);
			}

			break;

		case kControl:
		{
			if (chip.selected >= kChannels)
			{
				break;
			}

			const int channel = chip.selected;
			const uint8_t previous = chip.control[channel];
			chip.control[channel] = (uint8_t) value;

			// Turning DDA off puts the table's read pointer back to zero (`c6280_mame.c:219-222`),
			// which is how a driver loads a fresh table for the next note.
			if ((previous & 0x40) != 0 && (value & 0x40) == 0)
			{
				chip.index[channel] = 0;
			}

			const bool wasKeyed = (previous & 0x80) != 0;
			chip.keyed[channel] = (value & 0x80) != 0;

			if (chip.keyed[channel] && ! wasKeyed)
			{
				emit (chip, channel);
			}

			break;
		}

		case kWaveRegister:
		{
			if (chip.selected >= kChannels)
			{
				break;
			}

			const int channel = chip.selected;

			// With DDA on, 0x06 carries a sample level rather than a table entry; this collector is
			// about tables, and mixing the two would corrupt the shape the row draws too.
			if ((chip.control[channel] & 0x40) != 0)
			{
				break;
			}

			chip.wave[channel][chip.index[channel]] = (uint8_t) (value & 0x1F);

			// Only a silent channel advances the index - the core loads a table while the voice is
			// off, so an index that moved while keyed on would put every later write one step off.
			if ((chip.control[channel] & 0x80) == 0)
			{
				chip.index[channel] = (chip.index[channel] + 1) % kTableBytes;
			}

			break;
		}

		default:
			break;		// 0x01 balance, 0x05 channel balance, 0x07 noise, 0x08/0x09 LFO
		}
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
