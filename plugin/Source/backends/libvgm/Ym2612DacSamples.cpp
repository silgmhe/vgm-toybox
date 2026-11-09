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
	/** A byte this close to 0x80 is the DAC's silence. Used to decide which intervals describe a
		drum, not to cut anything. */
	constexpr int kQuietLevel = 2;

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

	/** **Calibration against a known drum: 1.637.**

		The median interval understates the rate a bank is played at - measured by the user, who compared
		a snare from the song against the same snare in an exported PCM and found the file 1.637x too
		long. The file's waits are quantised to whole ticks (2 here), and the drum bodies sit between
		them; this factor is what that quantisation costs. It is a measurement, not a derivation: a song
		whose drums disagree will need the factor revisited, and the number is stated once so that is a
		one-line change rather than a re-investigation.
	*/
	constexpr double kDacRateCalibration = 1.637;

	/** **The rate a drum runs at**, taken as the median interval that follows a non-silence byte.

		Not the mean over the whole stream: a Mega Drive driver feeds silence at 1..2 ticks and drum
		bytes at 5..6, so the stream's mean (13 799 Hz on `02 Green Hill Zone`) sits between the two
		and matches neither. The median over the loud bytes is what a drum in this file actually ran
		at, and a median rather than a mean because a handful of long gaps must not drag it down.
	*/
	double drumRate (const std::vector<Hit>& hits, double tickRate)
	{
		std::vector<uint32_t> intervals;

		for (std::size_t i = 1; i < hits.size (); ++i)
		{
			if (isQuiet (hits[i - 1].value))
			{
				continue;
			}

			const uint32_t dt = hits[i].time - hits[i - 1].time;

			if (dt > 0 && dt <= 64)
			{
				intervals.push_back (dt);
			}
		}

		if (intervals.empty ())
		{
			return 0.0;
		}

		const std::size_t middle = intervals.size () / 2;
		std::nth_element (intervals.begin (), intervals.begin () + middle, intervals.end ());

		const uint32_t median = intervals[middle];
		return median > 0 ? tickRate * kDacRateCalibration / (double) median : 0.0;
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

		const double rate = drumRate (hits, ir.tickRate);

		if (rate <= 0.0)
		{
			continue;
		}

		char name[48] = {};
		std::vector<uint8_t> bytes;

		if (! bank.empty ())
		{
			// **The whole bank, one file.** Cutting it into hits was tried and rejected: the
			// boundaries are the driver's business, and a pause heuristic produced 11 fragments of
			// 3 ms to 165 ms that nobody would call drums. The bank is what the file carries, and a
			// sampler user can cut it where the music does.
			bytes = bank;
		}
		else
		{
			// No block: the bytes came from the driver's own memory, so the stream is all there is.
			bytes.reserve (hits.size () );

			for (const auto& hit : hits)
			{
				bytes.push_back (hit.value);
			}
		}

		if (devices > 1)
		{
			std::snprintf (name, sizeof (name), "ym2612dac_%06X_%05X_d%u", 0u,
						   (unsigned) bytes.size (), (unsigned) index);
		}
		else
		{
			std::snprintf (name, sizeof (name), "ym2612dac_%06X_%05X", 0u, (unsigned) bytes.size ());
		}

		collector.add (contentHash (bytes.data (), bytes.size (), index), name, bytes.data (),
					   bytes.size (), SampleFormat::pcm8Unsigned, rate);
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
