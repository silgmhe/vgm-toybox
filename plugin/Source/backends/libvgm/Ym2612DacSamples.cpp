#include "Ym2612DacSamples.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

#include "SampleCollector.h"
#include <memory>
#include <memory>
#include "Ym2612DacSampler.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	/** A DAC bank larger than this is not a Mega Drive bank; the ceiling keeps a broken header from
		allocating gigabytes. */
	constexpr std::size_t kMaxBankBytes = 4u << 20;

	/** The smallest range worth a file: one byte is a level, not a sample. */
	constexpr std::size_t kMinSampleBytes = 2;

	/** **A key that names the run, not its bytes.**

		The collector drops a second sample with the same key, so hashing the content would fold two
		different entries into one whenever a driver stores the same drum twice - which `04 Spring Yard
		Zone` does (3 808 identical bytes at `0x0000` and `0x0EE0`), and `03 Marble Zone` does with two
		seven-byte silence entries (`0x1584`, `0x111D`). The reference keys its map on `(start, length)`
		and keeps both, and the export is meant to be the same set.
	*/
	uint64_t sampleKey (std::size_t device, uint32_t start, uint32_t length)
	{
		uint64_t key = 1469598103934665603ull;
		key = (key ^ (uint64_t) device) * 1099511628211ull;
		key = (key ^ (uint64_t) start) * 1099511628211ull;
		key = (key ^ (uint64_t) length) * 1099511628211ull;
		return key;
	}
}

std::vector<playback::DecodedSample> collectYm2612DacSamples (const midi::SongIr& ir,
														  const dac::Table* table)
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

	if (devices == 0)
	{
		return collector.take ();
	}

	// **The table the song already built.** `LibvgmSongInstance` builds it once for the track players
	// (and the row), and this pass only maps its runs to files - the scan over 459 546 writes would
	// otherwise run a second time. A caller that has no table still gets the same answer: built here.
	std::shared_ptr<const dac::Table> owned;

	if (table == nullptr)
	{
		owned = dac::buildTable (ir, kMaxBankBytes);
		table = owned.get ();
	}

	if (table == nullptr || table->bank.empty ())
	{
		return collector.take ();
	}

	const std::vector<uint8_t>& bank = table->bank;
	const uint32_t tickRate = table->tickRate;

	for (std::size_t device = 0; device < ir.chips.size (); ++device)
	{
		if (ir.chips[device].kind != midi::ChipKind::ym2612)
		{
			continue;
		}

		// The table belongs to one chip: the one whose stream it was built from. A hypothetical
		// second YM2612 would need its own, and the format carries no seek for it.
		if (device != table->device)
		{
			continue;
		}

		const std::vector<dac::Run>& runs = table->runs;

		// `(start, length)` names a sample: the same bytes played for the same run are one file,
		// and its rate is the first one measured - the reference's rule, so the sets match.
		//
		// **The number in the name comes from the table**, `assignSampleIndices`'s first-played
		// order, because the row shows the same number as a note: a name and a note that disagreed
		// would make the panel useless for telling one drum from another.
		std::map<std::pair<uint32_t, uint32_t>, bool> seen;

		for (const auto& run : runs)
		{
			if (run.sample == dac::kNoSample)
			{
				continue;			// the table already decided this run is not a sample
			}

			if (run.start >= bank.size ())
			{
				continue;
			}

			const std::size_t length = (std::min<std::size_t>) (run.length, bank.size () - run.start);

			if (length < kMinSampleBytes)
			{
				continue;
			}

			if (! seen.emplace (std::make_pair (run.start, (uint32_t) length), true).second)
			{
				continue;
			}

			// **The rate is an integer division, exactly as the reference computes it**, so the
			// number in the WAV header equals the number in a `.fur` sample for the same run.
			const uint64_t ticks = (uint64_t) length * (uint64_t) tickRate;
			const uint32_t rate = (uint32_t) (ticks / run.duration);

			if (rate == 0)
			{
				continue;
			}

			// **Signed, because the DAC's silence is 0x80**: the WAV payload then matches a `.fur`
			// sample's bytes exactly, and a reader that assumes unsigned shifts everything by half
			// the range on the way in.
			std::vector<uint8_t> bytes (length);

			for (std::size_t i = 0; i < length; ++i)
			{
				bytes[i] = (uint8_t) (bank[run.start + i] - 0x80);
			}

			char name[64] = {};

			if (devices > 1)
			{
				std::snprintf (name, sizeof (name), "ym2612dac_%02u_%06X_%05X_d%u", (unsigned) run.sample,
							   (unsigned) run.start, (unsigned) length, (unsigned) device);
			}
			else
			{
				std::snprintf (name, sizeof (name), "ym2612dac_%02u_%06X_%05X", (unsigned) run.sample,
							   (unsigned) run.start, (unsigned) length);
			}

			collector.add (sampleKey (device, run.start, (uint32_t) length), name, bytes.data (),
						   length, SampleFormat::pcm8Signed, (double) rate);
		}
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
