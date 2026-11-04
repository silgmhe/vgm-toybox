#include "K007232Samples.h"

#include <algorithm>
#include <array>
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
	/** The K007232's ROM, as VGM calls it - the type its `kRequirements` names, so the image here
		is the one the decoder was handed. */
	constexpr uint8_t kRomType = 0x94;

	/** Its addressable limit: the bank register shifts by seventeen bits, and the image a song
		declares is what `memory::rom` clamps to anyway. */
	constexpr std::size_t kMaxRomBytes = 8u << 20;

	/** One bank window: seventeen address bits, which the bank register adds to. */
	constexpr uint32_t kWindowBytes = 0x20000u;

	/** `K007232_CLOCKDIV` - the device's own rate is the clock divided by this. */
	constexpr double kClockDivisor = 128.0;

	/** The counter walks this much per output sample and reloads with `0x1000 - step`. */
	constexpr double kCounterWalk = 32.0;
	constexpr double kCounterRange = 4096.0;

	constexpr uint32_t kDefaultClock = 8000000u;

	/** The step a voice is playing at, from its two pitch registers. Mirrors `k007232.c:274-291`,
		including the two frequency modes the core itself calls unimplemented. */
	uint16_t stepOf (const uint8_t* regs)
	{
		const uint8_t low = regs[0];
		const uint8_t high = regs[1];
		const uint8_t mode = (uint8_t) ((high >> 4) & 0x03);

		if (mode == 0x01)
		{
			return (uint16_t) ((256u - low) << 4);
		}

		if (mode == 0x02)
		{
			return (uint16_t) ((16u - (high & 0x0F)) << 8);
		}

		return (uint16_t) (((high & 0x0F) << 8) | low);
	}
}

std::vector<playback::DecodedSample> collectK007232Samples (const midi::SongIr& ir,
														   double chipRateHz)
{
	SampleCollector collector;

	const std::vector<uint8_t> rom = memory::rom (ir, kRomType, kMaxRomBytes, false);

	if (rom.empty ())
	{
		return {};
	}

	/** `clock / 128`, the rate the device reports. The per-note factor is applied per sample. */
	const double chipRate = (chipRateHz > 0.0) ? chipRateHz
											  : (double) kDefaultClock / kClockDivisor;

	struct Voice
	{
		uint8_t regs[6] = { 0, 0, 0, 0, 0, 0 };
		uint8_t bank = 0;
		uint32_t start = 0;
		bool keyed = false;
	};

	/** What a sample is, once its address is complete. The rate is kept apart because it is the
		one value that changes after the key-on - see the note where it is updated. */
	struct Pending
	{
		uint32_t base = 0;
		std::size_t length = 0;
		double rate = 0.0;
	};

	std::map<uint32_t, std::array<Voice, 2>> chips;
	std::map<uint64_t, Pending> pending;

	/** The key a voice's current sample is filed under: **bank and start, not the device.**
	 *
	 *	Every instance of a chip type is handed the same assembled image (`forRequirements` builds
	 *	one per type), so two chips naming the same window and address name the same bytes. Keeping
	 *	the device in the key filed `04 Main BGM`'s shared sample twice, under two identical names.
	 */
	const auto keyOf = [] (const Voice& voice)
	{
		return ((uint64_t) voice.bank << 17) | (uint64_t) voice.start;
	};

	/** Bytes per second at a step: the device rate times the counter's walk. Zero when the step
		sits at the top of the range, where the counter never reloads. */
	const auto rateFor = [chipRate] (uint16_t step)
	{
		const double remaining = kCounterRange - (double) step;
		return (remaining <= 0.0) ? 0.0 : chipRate * kCounterWalk / remaining;
	};

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::k007232)
		{
			continue;
		}

		auto& voices = chips[write.deviceId];

		if (write.reg >= 12)
		{
			switch (write.reg)
			{
			case 0x14:
			case 0x15:
				voices[(std::size_t) (write.reg & 1)].bank = write.value;
				break;

			default:
				break;		// 0x0C is the external port, 0x10..0x13 the volumes, 0x0D the loop mask
			}

			continue;
		}

		const int channel = write.reg / 6;
		const int offset = write.reg % 6;
		Voice& voice = voices[(std::size_t) channel];
		voice.regs[offset] = write.value;

		if (offset <= 0x01)
		{
			// **The rate can arrive after the key-on, and it can change during one.** A driver that
			// writes the start address first and the pitch second leaves the voice momentarily at
			// the previous note's step - taking the rate at the key-on alone filed most of
			// `02 Title Demo` at 218 Hz, the step-zero rate, which is seven seconds of audio for a
			// one-second sample. And the drivers here retune mid-sample: one voice reads 10528 Hz at
			// two seconds and 18643 at five.
			//
			// So the **last** write wins, which is the value the row reports and therefore the one
			// the file has to declare. A file can only carry one rate; the alternative is a file per
			// retune of the same bytes, and the bytes are what a sample is.
			const auto found = pending.find (keyOf (voice));

			if (found != pending.end ())
			{
				found->second.rate = rateFor (stepOf (voice.regs));
			}

			continue;
		}

		// The three address bytes arrive one at a time, low to high, so only the write to the high
		// byte completes an address. Emitting on each of them filed `...064B3` and `...064B6` for
		// one sample, three bytes apart.
		const bool completesAddress = (offset == 0x04);
		const bool keysOn = (offset == 0x05) && ((write.value & 0x01) != 0);

		if (completesAddress || keysOn)
		{
			if (completesAddress)
			{
				voice.start = ((uint32_t) (voice.regs[4] & 0x01) << 16)
							| ((uint32_t) voice.regs[3] << 8) | (uint32_t) voice.regs[2];
			}

			voice.keyed = true;

			const uint32_t base = ((uint32_t) voice.bank << 17) + (voice.start & 0x1FFFFu);

			if (base < rom.size ())
			{
				// **The length is in the data.** `k007232.c:232` plays a byte as
				// `(value & 0x7F) - 0x40`; `k007232.c:246` treats bit 7 as the end marker, so the
				// terminator is not a level.
				std::size_t length = 0;

				while (base + length < rom.size () && length < kWindowBytes
					   && (rom[base + length] & 0x80u) == 0)
				{
					++length;
				}

				if (length >= 2)
				{
					Pending& entry = pending[keyOf (voice)];

					// The first key-on fixes the bytes - the ROM is immutable, so every later one
					// names the same recording. The rate is refreshed every time, for the same
					// reason the pitch writes refresh it: the last value is the settled one.
					if (entry.length == 0)
					{
						entry.base = base;
						entry.length = length;
					}

					entry.rate = rateFor (stepOf (voice.regs));
				}
			}

			continue;
		}

		// The low and middle address bytes only accumulate; nothing is complete yet.
	}

	// **Filed at the end, not on the way through.** A sample's rate may still change after its
	// key-on, and `add` keeps the first entry for a key - so the decision has to wait until every
	// write has been seen.
	for (const auto& [key, entry] : pending)
	{
		if (entry.length < 2 || entry.rate <= 0.0)
		{
			continue;
		}

		const uint8_t bank = (uint8_t) ((key >> 17) & 0xFFu);
		const uint32_t start = (uint32_t) (key & 0x1FFFFu);

		char name[48] = {};
		std::snprintf (name, sizeof (name), "k007232_%u_%05X", (unsigned) bank, (unsigned) start);

		collector.add (key, name, rom.data () + entry.base, entry.length, SampleFormat::k007232,
					   entry.rate);
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
