#include "Es5503Samples.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>

#include "ChipMemoryAssembly.h"
#include "SampleCollector.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	/** The DOC's RAM, as VGM calls it: a **RAM block** (0xC0-0xFF), whose payload starts with a
		thirty-two bit address rather than a ROM size and offset. */
	constexpr uint8_t kRamType = 0xE1;

	/** The chip addresses 64 KB; the assembler's ceiling is what the decoder is handed as well. */
	constexpr std::size_t kMaxRamBytes = 128u << 10;

	/** `es5503.c:184` - the size register's low three bits index this, so the value is not bytes. */
	constexpr uint32_t kTableSizes[8] = { 256, 512, 1024, 2048, 4096, 8192, 16384, 32768 };

	/** `es5503.c:185` - the mask that keeps a table inside the 128 KB the pointer can name. */
	constexpr uint32_t kTableMasks[8] =
		{ 0x1ff00, 0x1fe00, 0x1fc00, 0x1f800, 0x1f000, 0x1e000, 0x1c000, 0x18000 };

	/** `es5503.c:189` - the chip's sample rate is the master clock over eight. */
	constexpr double kClockDivisor = 8.0;

	constexpr uint32_t kDefaultClock = 7159090u;

	/** One oscillator's registers, as far as a sample is concerned. */
	struct Voice
	{
		uint16_t frequency = 0;
		uint32_t tablePointer = 0;
		uint8_t sizeIndex = 0;
		uint8_t resolution = 0;
		uint8_t control = 0;
		bool keyed = false;

		/** What the voice latched at its key-on, so a later write retunes the sample that is
			actually playing rather than the one being prepared. */
		uint32_t latchBase = 0;
		uint8_t latchSize = 0;
	};

	struct Pending
	{
		std::vector<uint8_t> bytes;
		std::size_t length = 0;
		double rate = 0.0;
	};

	/** The power-of-two mask the decoder folds an address with. */
	uint32_t pow2Mask (std::size_t size)
	{
		if (size == 0)
		{
			return 0;
		}

		uint32_t mask = (uint32_t) (size - 1);
		mask |= mask >> 1;
		mask |= mask >> 2;
		mask |= mask >> 4;
		mask |= mask >> 8;
		mask |= mask >> 16;
		return mask;
	}
}

std::vector<playback::DecodedSample> collectEs5503Samples (const midi::SongIr& ir, uint32_t clockHz)
{
	SampleCollector collector;

	const std::vector<uint8_t> ram = memory::ram (ir, kRamType, kMaxRamBytes);

	if (ram.empty ())
	{
		return {};
	}

	const uint32_t ramMask = pow2Mask (ram.size ());

	const auto byteAt = [&ram, ramMask] (uint32_t address)
	{
		const uint32_t index = address & ramMask;
		return (index < ram.size ()) ? ram[index] : (uint8_t) 0;
	};

	const double clock = (double) ((clockHz != 0) ? clockHz : kDefaultClock);

	/** `es5503_get_sample_rate`: a chip-wide number that 0xE1 moves for every voice at once. */
	const auto chipRate = [&clock] (int enabled)
	{
		return (clock / kClockDivisor) / (double) (enabled + 2);
	};

	/** Bytes per second this oscillator walks: `freq` accumulator units per chip sample, and the
		index is `accumulator >> (9 + resolution - size)`. */
	const auto rateFor = [&chipRate] (const Voice& voice, int enabled)
	{
		const double step = std::ldexp (1.0, 9 + (int) voice.resolution - (int) voice.sizeIndex);
		return chipRate (enabled) * (double) voice.frequency / step;
	};

	const auto keyOf = [] (uint32_t base, uint8_t sizeIndex)
	{
		return ((uint64_t) base << 3) | (uint64_t) (sizeIndex & 0x07);
	};

	struct Chip
	{
		std::array<Voice, 32> voices;
		int enabled = 1;		// register 0xE1, 1..32
	};

	std::map<uint32_t, Chip> chips;
	std::map<uint64_t, Pending> pending;

	/** Files a keyed voice's table. The address and size are the identity: one RAM image is shared
		by every instance of the type, exactly as the K007232's image is. */
	const auto emit = [&] (Voice& voice, int enabled)
	{
		const uint32_t base = voice.tablePointer & kTableMasks[voice.sizeIndex & 0x07];
		const uint32_t tableSize = kTableSizes[voice.sizeIndex & 0x07];

		if (byteAt (base) == 0x00)
		{
			return;		// the core halts the oscillator before it produces a level
		}

		std::size_t length = 0;

		while (length < tableSize && byteAt (base + (uint32_t) length) != 0x00)
		{
			++length;
		}

		// A table with no marker is fine here - unlike the sampled chips, this one *ends* at its
		// table size in the core too - so only an empty one is refused.
		if (length < 2)
		{
			return;
		}

		const uint64_t key = keyOf (base, voice.sizeIndex);
		Pending& entry = pending[key];

		if (entry.length == 0)
		{
			entry.bytes.assign (ram.begin () + (long) base, ram.begin () + (long) (base + length));
			entry.length = length;
		}

		// Refreshed by every later key-on, and by every later pitch write: a driver may write the
		// step after the control byte, and 0xE1 can move the rate under a playing voice.
		entry.rate = rateFor (voice, enabled);
	};

	/** Retunes one voice's current sample, for the writes that change its speed. */
	const auto retune = [&] (Voice& voice, int enabled)
	{
		if (! voice.keyed)
		{
			return;
		}

		const auto found = pending.find (keyOf (voice.latchBase, voice.latchSize));

		if (found != pending.end ())
		{
			found->second.rate = rateFor (voice, enabled);
		}
	};

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::es5503)
		{
			continue;
		}

		Chip& chip = chips[write.deviceId];

		// 0xE0 upward are the chip-wide registers; 0xE1 is the only one a sample cares about.
		if (write.reg >= 0xE0)
		{
			if (write.reg == 0xE1 && write.value != 0)
			{
				// The number of oscillators is `(value >> 1) + 1`, and it changes the sample rate
				// for every voice on the chip - so every playing sample's declared rate moves.
				chip.enabled = ((write.value >> 1) & 0x1F) + 1;

				for (auto& voice : chip.voices)
				{
					retune (voice, chip.enabled);
				}
			}

			continue;
		}

		const int index = write.reg & 0x1F;
		const uint8_t page = (uint8_t) (write.reg & 0xE0);
		Voice& voice = chip.voices[(std::size_t) index];

		switch (page)
		{
		case 0x00:		// frequency, low
			voice.frequency = (uint16_t) ((voice.frequency & 0xFF00) | (write.value & 0xFF));
			retune (voice, chip.enabled);
			break;

		case 0x20:		// frequency, high
			voice.frequency = (uint16_t) ((voice.frequency & 0x00FF) | ((uint16_t) write.value << 8));
			retune (voice, chip.enabled);
			break;

		case 0x40:		// volume: the row reports it, the file has no use for it
			break;

		case 0x60:		// the current data byte, read-only
			break;

		case 0x80:		// table pointer, bits 8..15
			voice.tablePointer = (uint32_t) write.value << 8;
			break;

		case 0xA0:		// control: bit 0 is the halt bit, so a falling edge is a key-on
		{
			const bool wasHalted = (voice.control & 0x01) != 0;
			voice.control = (uint8_t) write.value;

			if (wasHalted && (write.value & 0x01) == 0)
			{
				voice.latchBase = voice.tablePointer & kTableMasks[voice.sizeIndex & 0x07];
				voice.latchSize = voice.sizeIndex;
				voice.keyed = true;
				emit (voice, chip.enabled);
			}
			else if ((write.value & 0x01) != 0)
			{
				voice.keyed = false;
			}

			break;
		}

		case 0xC0:		// bank bit, table size, resolution
			voice.tablePointer = (write.value & 0x40) ? (voice.tablePointer | 0x10000u)
													  : (voice.tablePointer & 0xFFFFu);
			voice.sizeIndex = (uint8_t) ((write.value >> 3) & 0x07);
			voice.resolution = (uint8_t) (write.value & 0x07);
			retune (voice, chip.enabled);
			break;

		default:
			break;
		}
	}

	for (auto& [key, entry] : pending)
	{
		if (entry.length < 2 || entry.rate <= 0.0)
		{
			continue;
		}

		char name[32] = {};
		std::snprintf (name, sizeof (name), "es5503_%05X", (unsigned) (key >> 3));

		collector.add (key, name, entry.bytes.data (), entry.length, SampleFormat::es5503,
					   entry.rate);
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
