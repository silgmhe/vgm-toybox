#include "OpnAdpcmSamples.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <vector>

#include "ChipMemoryAssembly.h"
#include "SampleCollector.h"
#include "chips/ChipTypes.h"

namespace toybox::backends::libvgm
{
namespace
{
	/** ADPCM-A's image. `0x83` is the Delta-T block - swapping the two decodes drums as noise. */
	constexpr uint8_t kRomType = 0x82;
	constexpr std::size_t kMaxRomBytes = 8u << 20;
	constexpr double kDefaultClock = 8000000.0;

	struct Voice
	{
		uint8_t startLow = 0;
		uint8_t startHigh = 0;
		uint8_t endLow = 0;
		uint8_t endHigh = 0;
	};

	struct Pending
	{
		uint32_t base = 0;
		std::size_t length = 0;
		double rate = 0.0;
	};
}

std::vector<playback::DecodedSample> collectOpnAdpcmSamples (const midi::SongIr& ir,
															 uint32_t clockHz)
{
	SampleCollector collector;

	const std::vector<uint8_t> rom = memory::rom (ir, kRomType, kMaxRomBytes, false);

	if (rom.empty ())
	{
		return {};
	}

	const double clock = (clockHz > 0) ? (double) clockHz : kDefaultClock;

	// One nibble every three DAC ticks: 8 MHz / 432 = 18.5 kHz, the rate these drums are recorded
	// at. `clock / 144` is three times too fast, which is what made an exported wave sound sharp.
	const double rate = clock / 432.0;

	std::size_t deviceSlot = ir.chips.size ();

	for (std::size_t i = 0; i < ir.chips.size (); ++i)
	{
		const midi::ChipKind kind = ir.chips[i].kind;

		if (kind == midi::ChipKind::ym2610 || kind == midi::ChipKind::ym2608)
		{
			deviceSlot = i;
			break;
		}
	}

	if (deviceSlot >= ir.chips.size ())
	{
		return {};
	}

	Voice voices[6];
	std::map<uint64_t, Pending> pending;

	for (const auto& write : ir.writes)
	{
		// ADPCM-A is on the second port; the first carries FM and the chip's key-on register.
		if (write.deviceId != (uint32_t) deviceSlot || write.port == 0 || write.reg >= 0x30)
		{
			continue;
		}

		const uint8_t reg = write.reg;
		const int slot = (int) (reg & 0x07);

		if (reg == 0x00)
		{
			const bool allOff = (write.value & 0x80) != 0;

			for (int channel = 0; channel < 6; ++channel)
			{
				if (allOff || (write.value & (1u << channel)) == 0)
				{
					continue;
				}

				const uint32_t begin = (((uint32_t) voices[channel].startHigh << 8)
										| voices[channel].startLow) << 8;
				const uint32_t end = ((((uint32_t) voices[channel].endHigh << 8)
									   | voices[channel].endLow) << 8) + 0xFF;

				if (end <= begin || begin >= rom.size ())
				{
					continue;
				}

				const std::size_t length = (std::size_t) std::min<uint32_t> (
					end - begin, (uint32_t) (rom.size () - begin));

				const uint64_t key = ((uint64_t) begin << 32) | (uint64_t) (end >> 8);
				Pending& entry = pending[key];

				if (entry.length == 0 || length > entry.length)
				{
					entry.base = begin;
					entry.length = length;
					entry.rate = rate;
				}
			}

			continue;
		}

		if (reg >= 0x10 && reg <= 0x15)
		{
			voices[slot].startLow = (uint8_t) write.value;
		}
		else if (reg >= 0x18 && reg <= 0x1D)
		{
			voices[slot].startHigh = (uint8_t) write.value;
		}
		else if (reg >= 0x20 && reg <= 0x25)
		{
			voices[slot].endLow = (uint8_t) write.value;
		}
		else if (reg >= 0x28 && reg <= 0x2D)
		{
			voices[slot].endHigh = (uint8_t) write.value;
		}
	}

	// **One page is the granularity the chip has.** Start and end are written as `reg << 8`, so a
	// retrigger of the same recording often lands on the neighbouring page and every hit produced
	// its own file - 94 exports for a drum kit of a few dozen sounds. Ranges that overlap or sit
	// within a page of each other are the same recording and are joined; anything further apart may
	// be a different drum, and merging that would splice two sounds together the way the Sega PCM
	// merge did.
	struct Span
	{
		uint32_t begin = 0;
		uint32_t end = 0;
		double rate = 0.0;
	};

	std::vector<Span> spans;
	spans.reserve (pending.size ());

	for (const auto& entry : pending)
	{
		if (entry.second.length < 2 || entry.second.rate <= 0.0)
		{
			continue;
		}

		spans.push_back (Span { entry.second.base, entry.second.base + (uint32_t) entry.second.length,
								entry.second.rate });
	}

	std::sort (spans.begin (), spans.end (),
			   [] (const Span& a, const Span& b) { return a.begin < b.begin; });

	// **Containment, not adjacency.** Joining ranges that merely sit close together chained a whole
	// packed bank into one 44-second file - the same failure the Sega PCM merge produced. A range
	// that is *inside* another is the same recording retriggered from a later page, so the wider one
	// already carries it and the narrower adds nothing.
	std::vector<Span> merged;

	std::sort (spans.begin (), spans.end (),
			   [] (const Span& a, const Span& b) { return a.begin < b.begin; });

	for (const auto& span : spans)
	{
		// **Join a retrigger, but never build a bank.** A page's worth of slack collects the hits
		// that re-write the same recording from the neighbouring page; the cap stops the chain at a
		// second or so of audio, because without it a packed bank merges into one 44-second file
		// holding every drum in it - the Sega PCM failure at a smaller scale.
		constexpr uint32_t kMergeGap = 0x100;
		constexpr uint32_t kMaxSpan = 0x4000;		// 16 KB, about 0.9 s at 18.5 kHz

		if (! merged.empty () && span.begin <= merged.back ().end + kMergeGap
			&& span.end - merged.back ().begin <= kMaxSpan)
		{
			merged.back ().end = std::max (merged.back ().end, span.end);
			merged.back ().rate = std::max (merged.back ().rate, span.rate);
			continue;
		}

		merged.push_back (span);
	}

	// **Then the bytes decide.** Two ranges can cover the same recording without their pages
	// touching; hashing the payload collapses those and never splices neighbours.
	std::map<uint64_t, uint32_t> seen;

	for (const auto& span : merged)
	{
		if (span.begin >= rom.size ())
		{
			continue;
		}

		const uint32_t length = (std::min<uint32_t>) (
			span.end - span.begin, (uint32_t) (rom.size () - span.begin));

		if (length < 2)
		{
			continue;
		}

		uint64_t hash = 1469598103934665603ull;

		for (uint32_t i = 0; i < length; ++i)
		{
			hash = (hash ^ rom[span.begin + i]) * 1099511628211ull;
		}

		hash ^= (uint64_t) length << 40;

		if (! seen.emplace (hash, span.begin).second)
		{
			continue;
		}

		char name[40] = {};
		std::snprintf (name, sizeof (name), "opnadpcm_%05X_%05X", (unsigned) span.begin,
					   (unsigned) length);

		// A nibble is one sample: the file's rate is the nibble rate and the bytes are decoded by
		// `adpcmA4`, two values per byte.
		collector.add (hash, name, rom.data () + span.begin, length, SampleFormat::adpcmA4,
					   span.rate);
	}

	return collector.take ();
}
}
