#include "Ym2612DacSamples.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "SampleCollector.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	/** A pause that ends a run when the file carries no PCM block: 30 ms. Twelve times the longest
		dither interval these drivers use (6 ticks) and shorter than any drum's decay. */
	constexpr double kSilenceSeconds = 0.030;

	/** A driver that writes without ever pausing must not produce an unbounded file. The bank path
		has its own cap; this one bounds the stream path. */
	constexpr std::size_t kMaxRunBytes = 1u << 20;
	constexpr std::size_t kMaxBankBytes = 4u << 20;

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

	struct Hit
	{
		uint32_t time = 0;
		uint8_t value = 0;
	};

	double rateOf (const std::vector<Hit>& hits, std::size_t from, std::size_t count, double tickRate)
	{
		uint64_t ticks = 0;
		uint32_t intervals = 0;

		for (std::size_t i = 1; i < count; ++i)
		{
			const uint32_t dt = hits[from + i].time - hits[from + i - 1].time;

			if (dt > 0 && dt <= 64)
			{
				ticks += dt;
				++intervals;
			}
		}

		return (intervals > 0 && ticks > 0) ? tickRate * (double) intervals / (double) ticks : 0.0;
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

	// **One PCM block per file, not one per chip.** A 32X set has two YM2612s and both read the
	// same `0x67` bank, so building it once is both cheaper and the truth.
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

		if (! bank.empty ())
		{
			// **The bank is the sample.** The file carries a loop of drum material, not a table of
			// one-shots: `02 Green Hill Zone`'s 13 132 bytes are played at 8 495 Hz and sought into
			// 228 times at five offsets. Exporting the bank gives the material once; slicing it by
			// silence was measured and produces one 1.55 s segment, i.e. no boundaries at all.
			const double rate = rateOf (hits, 0, hits.size (), ir.tickRate);

			if (rate <= 0.0)
			{
				continue;
			}

			char name[48] = {};

			if (devices > 1)
			{
				std::snprintf (name, sizeof (name), "ym2612dac_%06X_%05X_d%u", 0u,
							   (unsigned) bank.size (), (unsigned) index);
			}
			else
			{
				std::snprintf (name, sizeof (name), "ym2612dac_%06X_%05X", 0u,
							   (unsigned) bank.size ());
			}

			collector.add (contentHash (bank.data (), bank.size (), index), name, bank.data (),
						   bank.size (), SampleFormat::pcm8Unsigned, rate);
			continue;
		}

		// **No block: the bytes came from the driver's own memory.** Then the only structure left is
		// the pauses between hits, and a run between two of them is the closest thing to a sample
		// this file has.
		const uint32_t silenceTicks = (uint32_t) (ir.tickRate * kSilenceSeconds);
		std::size_t start = 0;

		for (std::size_t i = 1; i <= hits.size (); ++i)
		{
			const bool cut = (i == hits.size ())
							 || (hits[i].time - hits[i - 1].time >= silenceTicks);

			if (! cut)
			{
				continue;
			}

			const std::size_t length = (std::min) (i - start, kMaxRunBytes);

			if (length >= 16)
			{
				std::vector<uint8_t> bytes;
				bytes.reserve (length);

				for (std::size_t k = start; k < start + length; ++k)
				{
					bytes.push_back (hits[k].value);
				}

				const double rate = rateOf (hits, start, length, ir.tickRate);

				if (rate > 0.0)
				{
					char name[48] = {};

					if (devices > 1)
					{
						std::snprintf (name, sizeof (name), "ym2612dac_%06X_%05X_d%u",
									   (unsigned) start, (unsigned) length, (unsigned) index);
					}
					else
					{
						std::snprintf (name, sizeof (name), "ym2612dac_%06X_%05X",
									   (unsigned) start, (unsigned) length);
					}

					collector.add (contentHash (bytes.data (), bytes.size (), index), name,
								   bytes.data (), bytes.size (), SampleFormat::pcm8Unsigned, rate);
				}
			}

			start = i;
		}
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
