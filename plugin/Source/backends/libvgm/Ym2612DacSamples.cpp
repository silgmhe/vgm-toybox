#include "Ym2612DacSamples.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

#include "SampleCollector.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	/** A byte this close to 0x80 is the DAC's silence; used to pick the intervals that describe a
		drum, never to cut anything. */
	constexpr int kQuietLevel = 2;

	constexpr std::size_t kMinRegionBytes = 16;
	constexpr std::size_t kMaxBankBytes = 4u << 20;

	struct Hit
	{
		uint32_t time = 0;
		uint8_t value = 0;
	};

	bool isQuiet (uint8_t value)
	{
		return std::abs ((int) value - 0x80) <= kQuietLevel;
	}

	uint64_t contentHash (const uint8_t* bytes, std::size_t length, std::size_t device)
	{
		uint64_t hash = 1469598103934665603ull;
		hash = (hash ^ (uint64_t) device) * 1099511628211ull;

		for (std::size_t i = 0; i < length; ++i)
		{
			hash = (hash ^ bytes[i]) * 1099511628211ull;
		}

		return hash ^ ((uint64_t) length << 40);
	}

	/** The rate a set of intervals names: the mean, in bytes a second.

		**The mean and not the median.** A stream that wants 5.5 ticks alternates 5 and 6, and the
		median of that is a whole number - 8 820 Hz where the bytes were written at 7 941. The mean
		recovers the fraction the dither encodes. Long gaps are already excluded (a pause is not an
		interval) and a region's visits stay within 0.02 ticks of each other, so there is nothing for
		a mean to be dragged by.
	*/
	double rateOf (const std::vector<uint32_t>& intervals, double tickRate)
	{
		if (intervals.empty ())
		{
			return 0.0;
		}

		uint64_t ticks = 0;

		for (const uint32_t dt : intervals)
		{
			ticks += dt;
		}

		return ticks > 0 ? tickRate * (double) intervals.size () / (double) ticks : 0.0;
	}
}

std::vector<playback::DecodedSample> collectYm2612DacSamples (const midi::SongIr& ir)
{
	SampleCollector collector;

	std::size_t devices = 0;

	for (const auto& slot : ir.chips)
	{
		if (slot.kind == midi::ChipKind::ym2612)
		{
			++devices;
		}
	}

	// **One PCM block per file, not one per chip.** A 32X set has two YM2612s and both read the same
	// `0x67` bank, so building it once is both cheaper and the truth.
	std::vector<uint8_t> bank;

	for (const auto& block : ir.roms)
	{
		if (bank.size () >= kMaxBankBytes)
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

		const std::size_t count = (std::min) (block.size, kMaxBankBytes - bank.size ());
		bank.insert (bank.end (), block.data, block.data + count);
	}

	for (std::size_t index = 0; index < ir.chips.size (); ++index)
	{
		if (ir.chips[index].kind != midi::ChipKind::ym2612)
		{
			continue;
		}

		std::vector<Hit> hits;

		for (const auto& write : ir.writes)
		{
			if ((std::size_t) write.deviceId == index && write.port == 0 && write.reg == 0x2A)
			{
				hits.push_back (Hit { write.sampleTime, (uint8_t) write.value });
			}
		}

		if (hits.size () < 2)
		{
			continue;			// no stream, nothing to measure a rate from
		}

		/** **The unit is the region the driver seeks to, and its rate is the region's own.**

			The YM2612 has no sample table in registers; its table is the set of `0xE0` offsets a
			driver uses, and each entry is written at one fixed rate - on `02 Green Hill Zone` a visit
			to one offset averages 1.90 ticks and to another 5.55, and neither varies by more than
			0.02 between visits. A single rate for the song therefore matched half of it at best; that
			was the bug behind "the other songs are still wrong".
		*/
		std::vector<uint32_t> offsets;

		for (const auto& seek : ir.dacSeeks)
		{
			if (seek.offset < bank.size () && (seek.offset % 1) == 0)
			{
				offsets.push_back (seek.offset);
			}
		}

		std::sort (offsets.begin (), offsets.end ());
		offsets.erase (std::unique (offsets.begin (), offsets.end ()), offsets.end ());

		if (offsets.empty () || bank.empty ())
		{
			// No seek table and no bank: the whole stream is the only material there is, at the rate
			// its drum bytes run at.
			std::vector<uint32_t> intervals;

			for (std::size_t i = 1; i < hits.size (); ++i)
			{
				const uint32_t dt = hits[i].time - hits[i - 1].time;

				if (! isQuiet (hits[i - 1].value) && dt > 0 && dt <= 64)
				{
					intervals.push_back (dt);
				}
			}

			const double rate = rateOf (intervals, ir.tickRate);

			if (rate <= 0.0)
			{
				continue;
			}

			std::vector<uint8_t> bytes;

			if (! bank.empty ())
			{
				bytes = bank;
			}
			else
			{
				for (const auto& hit : hits)
				{
					bytes.push_back (hit.value);
				}
			}

			char name[48] = {};
			std::snprintf (name, sizeof (name), "ym2612dac_%06X_%05X", 0u, (unsigned) bytes.size ());
			collector.add (contentHash (bytes.data (), bytes.size (), index), name, bytes.data (),
						   bytes.size (), SampleFormat::pcm8Unsigned, rate);
			continue;
		}

		// Assign every byte to the last seek at or before it, then measure each region's own rate.
		std::map<uint32_t, std::vector<uint32_t>> intervals;
		std::size_t seekIndex = 0;
		uint32_t current = offsets.front ();

		for (std::size_t i = 0; i < hits.size (); ++i)
		{
			while (seekIndex < ir.dacSeeks.size () && ir.dacSeeks[seekIndex].sampleTime <= hits[i].time)
			{
				current = ir.dacSeeks[seekIndex].offset;
				++seekIndex;
			}

			if (i == 0 || hits[i].time < hits[i - 1].time)
			{
				continue;
			}

			const uint32_t dt = hits[i].time - hits[i - 1].time;

			if (! isQuiet (hits[i - 1].value) && dt > 0 && dt <= 64)
			{
				intervals[current].push_back (dt);
			}
		}

		std::vector<uint32_t> globalIntervals;

		for (auto& entry : intervals)
		{
			globalIntervals.insert (globalIntervals.end (), entry.second.begin (),
									entry.second.end ());
		}

		for (std::size_t i = 0; i < offsets.size (); ++i)
		{
			const uint32_t offset = offsets[i];

			if (offset >= bank.size ())
			{
				continue;
			}

			const uint32_t end = (i + 1 < offsets.size ()) ? offsets[i + 1] : (uint32_t) bank.size ();
			const std::size_t length = (std::min<std::size_t>) (end - offset,
																bank.size () - offset);

			if (length < kMinRegionBytes)
			{
				continue;
			}

			std::vector<uint32_t> own = intervals[offset];
			double rate = rateOf (own, ir.tickRate);

			if (rate <= 0.0)
			{
				// The driver seeks here but never leaves a loud byte - a silence region, or a table
				// entry this song only touches in passing. The song's own median is the fallback.
				std::vector<uint32_t> all = globalIntervals;
				rate = rateOf (all, ir.tickRate);
			}

			if (rate <= 0.0)
			{
				continue;
			}

			char name[48] = {};

			if (devices > 1)
			{
				std::snprintf (name, sizeof (name), "ym2612dac_%06X_%05X_d%u", (unsigned) offset,
							   (unsigned) length, (unsigned) index);
			}
			else
			{
				std::snprintf (name, sizeof (name), "ym2612dac_%06X_%05X", (unsigned) offset,
							   (unsigned) length);
			}

			const uint8_t* bytes = bank.data () + offset;
			collector.add (contentHash (bytes, length, index), name, bytes, length,
						   SampleFormat::pcm8Unsigned, rate);
		}
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
