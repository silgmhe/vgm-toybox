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
	/** **A pause this long ends a hit: 2 ms.** Measured on `02 Green Hill Zone`, where the stream is
		447 541 bytes and the drums sit in bursts: 2 ms yields 428 hits and 11 distinct recordings
		(the most repeated 95 times), while 8 ms merges them into 142 hits of which 6 are distinct,
		and 1 ms shatters each drum into its own decay segments. Two milliseconds is above the gaps
		inside a decaying drum and below the space between two hits. */
	constexpr double kQuietSeconds = 0.002;

	/** A byte this close to 0x80 is the DAC's silence. */
	constexpr int kQuietLevel = 2;

	/** A byte is "the same" as the one before it at this level of ear; used only to classify
		silence, not to compare recordings. */
	constexpr std::size_t kMinHitBytes = 16;
	constexpr std::size_t kMaxHitBytes = 1u << 18;

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

	/** One recording: its bytes and the rate the driver wrote them at.

		The rate is the mean interval **inside** this hit. Pauses are excluded (>64 ticks), because a
		number that averaged them would report the space between drums as part of the drum - the
		mistake that made the whole bank's rate 13.8 kHz when its drum bodies run at 8 kHz. */
	struct Run
	{
		std::vector<uint8_t> bytes;
		uint64_t ticks = 0;
		uint32_t intervals = 0;
		uint32_t lastTime = 0;
		bool haveLast = false;

		void add (uint32_t time, uint8_t value)
		{
			bytes.push_back (value);

			if (haveLast)
			{
				const uint32_t dt = time - lastTime;

				if (dt > 0 && dt <= 64)
				{
					ticks += dt;
					++intervals;
				}
			}

			lastTime = time;
			haveLast = true;
		}

		double rate (double tickRate) const
		{
			return (intervals > 0 && ticks > 0) ? tickRate * (double) intervals / (double) ticks : 0.0;
		}
	};
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

	const uint32_t quietTicks = (uint32_t) (ir.tickRate * kQuietSeconds);

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
			continue;
		}

		// **A hit is a run of non-silence between two pauses.** The YM2612 has no length register,
		// so this is the only boundary the file carries: the driver stops writing between drums and
		// streams 0x80 while it waits. The quiet bytes are not part of the recording, and the pause
		// is measured in ticks - a driver that writes silence quickly must not make every gap look
		// short.
		Run run;
		uint64_t quiet = 0;
		int emitted = 0;
		std::map<uint64_t, bool> seen;

		const auto close = [&] (void)
		{
			if (run.bytes.size () >= kMinHitBytes)
			{
				const double rate = run.rate (ir.tickRate);

				if (rate > 0.0)
				{
					char name[48] = {};

					if (devices > 1)
					{
						std::snprintf (name, sizeof (name), "ym2612dac_%02u_%05X_d%u", (unsigned) emitted,
									   (unsigned) run.bytes.size (), (unsigned) index);
					}
					else
					{
						std::snprintf (name, sizeof (name), "ym2612dac_%02u_%05X", (unsigned) emitted,
									   (unsigned) run.bytes.size ());
					}

					const uint64_t key = contentHash (run.bytes.data (), run.bytes.size (), index);

					if (seen.emplace (key, true).second)
					{
						collector.add (key, name, run.bytes.data (), run.bytes.size (),
									   SampleFormat::pcm8Unsigned, rate);
						++emitted;
					}
				}
			}

			run = Run();
		};

		for (std::size_t i = 0; i < hits.size (); ++i)
		{
			const Hit& hit = hits[i];
			const uint32_t dt = (i + 1 < hits.size ()) ? (hits[i + 1].time - hit.time) : 0;

			if (isQuiet (hit.value))
			{
				if (! run.bytes.empty ())
				{
					quiet += dt;

					if (quiet >= quietTicks)
					{
						close ();
						quiet = 0;
					}
				}

				continue;
			}

			// A loud byte after a pause starts the next hit; the run's own rate starts here.
			quiet = 0;

			if (run.bytes.size () >= kMaxHitBytes)
			{
				close ();
			}

			run.add (hit.time, hit.value);
		}

		close ();
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
