#include "Ym2612DacSamples.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

#include "SampleCollector.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	/** **A pause this long ends a hit**: 512 samples at 44.1 kHz = 11.6 ms.

		Measured on `02 Green Hill Zone`: the gaps inside one drum are shorter than this and the gaps
		between two hits are longer - and most hits are cut by the driver's own `0xE0` anyway, so this
		only has to separate the ones that are not. Two milliseconds was tried once and shattered
		decays into fragments (916 hits / 18 kinds); this value is the reference implementation's
		(`vgm2fur/chips/sampler.py:4`), which is also what makes our files match its `.fur`.
	*/
	constexpr uint32_t kSeparationSamples = 512;

	/** A DAC bank larger than this is not a Mega Drive bank; the ceiling keeps a broken header from
		allocating gigabytes. */
	constexpr std::size_t kMaxBankBytes = 4u << 20;

	/** The smallest range worth a file: one byte is a level, not a sample. */
	constexpr std::size_t kMinSampleBytes = 2;

	/** One thing that happened to the DAC: a seek, or a byte the stream delivered. */
	struct Event
	{
		uint32_t time = 0;
		uint32_t order = 0;		// stream position, for two events at one instant
		uint32_t offset = 0;	// seek only
		uint8_t value = 0;		// byte only
		bool seek = false;
	};

	/** One hit: the bytes the driver played between two boundaries, and how long it took. */
	struct Run
	{
		uint32_t start = 0;
		uint32_t length = 0;
		uint32_t duration = 0;	// ticks spent playing, pauses shorter than 11.6 ms included
	};

	bool wasEarlier (const Event& a, const Event& b)
	{
		if (a.time != b.time)
		{
			return a.time < b.time;
		}

		// A seek at the same instant comes first: the driver moves the pointer, then feeds it.
		if (a.seek != b.seek)
		{
			return a.seek;
		}

		return a.order < b.order;
	}

	/** **The sampler the reference uses, in ticks rather than seconds.**

		The YM2612 has no sample table: a hit is the bytes the driver actually played. The state below
		is `vgm2fur/chips/sampler.py` line for line, because the export is meant to be the same sample
		set that tool puts in a Furnace module - same boundaries, same lengths, same rates.

		Three rules carry all of it:

		  1. `0xE0` while a hit is in progress (or before any byte) starts a new hit; a `0xE0` that
			 only moves the pointer through silence does not, and drops the silent bytes it skipped;
		  2. a pause of 512 samples or more between two bytes ends the hit - the next byte begins at
			 `start + length`, which is where the driver continued;
		  3. the rate is `tickRate * length / duration`, where `duration` counts the small pauses
			 inside the hit but not the long one that ended it.

		**The region model this replaces was wrong in a way the numbers only now show**: the `0xE0`
		offsets are seek targets, not sample boundaries. Marble's `0x0A80` is played 1 693 bytes
		straight through two later offsets, and Labyrinth plays 3 808 bytes from 0 across the offset
		at 1 904. Cutting at the offsets also made each region's rate a blend of runs with different
		paces - Labyrinth's offset 0 measured 14.9 kHz where the runs from it are 23.1 kHz.
	*/
	class Sampler
	{
	public:
		explicit Sampler (uint32_t tickRate) : tickRate_ (tickRate) {}

		void seek (uint32_t time, uint32_t offset)
		{
			if (duration_ > 0 || idle_)
			{
				close ();
				start_ = offset;
				length_ = 0;
				duration_ = 0;
				idle_ = false;
			}
			else
			{
				// Only silence has been played since the last hit; move the pointer and forget the
				// bytes, rather than exporting a hit that is nothing but silence.
				start_ = offset;
				length_ = 0;
				idle_ = false;
			}

			pauseBase_ = time;
			haveBase_ = true;
		}

		void byte (uint32_t time)
		{
			if (length_ > 0 && haveBase_)
			{
				const uint32_t pause = time > pauseBase_ ? time - pauseBase_ : 0;

				if (pause > kSeparationSamples)
				{
					if (duration_ > 0)
					{
						close ();
						start_ += length_;
						length_ = 0;
						duration_ = 0;
					}
					else
					{
						start_ += length_;
						length_ = 0;
					}
				}
				else
				{
					duration_ += pause;
				}
			}

			++length_;
			pauseBase_ = time;
			haveBase_ = true;
			idle_ = false;
		}

		void finish (void)
		{
			close ();
		}

		const std::vector<Run>& runs (void) const
		{
			return runs_;
		}

		uint32_t tickRate (void) const
		{
			return tickRate_;
		}

	private:
		void close (void)
		{
			if (length_ > 0 && duration_ > 0)
			{
				runs_.push_back (Run { start_, length_, duration_ });
			}
		}

		uint32_t tickRate_ = 44100;
		std::vector<Run> runs_;
		uint32_t start_ = 0;
		uint32_t length_ = 0;
		uint32_t duration_ = 0;
		uint32_t pauseBase_ = 0;
		bool haveBase_ = false;
		bool idle_ = true;
	};

	/** The PCM bank: every `0x00`/`0x80` block, concatenated in stream order.

		The reference takes one block, the last one, because its input files carry one. A VGM bank
		can be uploaded in pieces - the uploads concatenate, that is what the format says - so this
		is the one place the two implementations deliberately differ.
	*/
	std::vector<uint8_t> buildBank (const midi::SongIr& ir)
	{
		std::vector<uint8_t> bank;

		for (const auto& block : ir.roms)
		{
			if (bank.size () >= kMaxBankBytes)
			{
				break;
			}

			// Type 0x00 is the plain PCM block; 0x80 is the same bytes tagged as ROM, and the reader
			// has already spent its eight-byte size/offset header.
			if (block.isRam || block.data == nullptr
				|| (block.memoryType != 0x00 && block.memoryType != 0x80))
			{
				continue;
			}

			const std::size_t count = (std::min) (block.size, kMaxBankBytes - bank.size ());
			bank.insert (bank.end (), block.data, block.data + count);
		}

		return bank;
	}

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

	if (devices == 0)
	{
		return collector.take ();
	}

	// **One bank per song, not one per chip.** A 32X set has two YM2612s and both read the same
	// `0x67` bank, so building it once is both cheaper and the truth.
	const std::vector<uint8_t> bank = buildBank (ir);

	if (bank.empty ())
	{
		return collector.take ();
	}

	const uint32_t tickRate = (uint32_t) std::llround (ir.tickRate > 0.0 ? ir.tickRate : 44100.0);

	for (std::size_t device = 0; device < ir.chips.size (); ++device)
	{
		if (ir.chips[device].kind != midi::ChipKind::ym2612)
		{
			continue;
		}

		// **Two inputs, one timeline.** The seeks say where the driver moved the pointer and the
		// stream bytes say what it fed it; the sampler needs both in order to know where a hit
		// starts and how long it ran. `0x52 0x2A` pokes are left out on purpose - they are stops and
		// level steps, and counting them would cut samples that the driver played as one.
		std::vector<Event> events;

		for (const auto& write : ir.writes)
		{
			if ((std::size_t) write.deviceId == device && write.port == 0 && write.reg == 0x2A
				&& write.fromStream)
			{
				Event event;
				event.time = write.sampleTime;
				event.order = write.streamOrder;
				event.value = (uint8_t) write.value;
				events.push_back (event);
			}
		}

		for (const auto& seek : ir.dacSeeks)
		{
			Event event;
			event.time = seek.sampleTime;
			event.offset = seek.offset;
			event.seek = true;
			events.push_back (event);
		}

		if (events.empty ())
		{
			continue;
		}

		std::stable_sort (events.begin (), events.end (), wasEarlier);

		Sampler sampler (tickRate);

		for (const auto& event : events)
		{
			if (event.seek)
			{
				sampler.seek (event.time, event.offset);
			}
			else
			{
				sampler.byte (event.time);
			}
		}

		sampler.finish ();

		// `(start, length)` names a sample: the same bytes played for the same run are one file,
		// and its rate is the first one measured - the reference's rule, so the sets match.
		std::map<std::pair<uint32_t, uint32_t>, bool> seen;
		unsigned number = 0;

		for (const auto& run : sampler.runs ())
		{
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
				std::snprintf (name, sizeof (name), "ym2612dac_%02u_%06X_%05X_d%u", number,
							   (unsigned) run.start, (unsigned) length, (unsigned) device);
			}
			else
			{
				std::snprintf (name, sizeof (name), "ym2612dac_%02u_%06X_%05X", number,
							   (unsigned) run.start, (unsigned) length);
			}

			collector.add (sampleKey (device, run.start, (uint32_t) length), name, bytes.data (),
						   length, SampleFormat::pcm8Signed, (double) rate);
			++number;
		}
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
