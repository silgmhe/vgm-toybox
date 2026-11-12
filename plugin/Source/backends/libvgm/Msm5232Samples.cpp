#include "Msm5232Samples.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

#include "SampleCollector.h"
#include "chips/Msm5232Shape.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	constexpr int kChannels = 8;
	constexpr int kFirstKey = 21;
	constexpr uint8_t kNoiseThreshold = 0xD8;

	double hzOf (int index)
	{
		return 440.0 * std::pow (2.0, (double) (kFirstKey + index - 69) / 12.0);
	}

	uint64_t hashOf (const std::vector<uint8_t>& bytes)
	{
		uint64_t hash = 1469598103934665603ull;

		for (const uint8_t byte : bytes)
		{
			hash ^= (uint64_t) byte;
			hash *= 1099511628211ull;
		}

		return hash;
	}

	struct Pending
	{
		std::vector<uint8_t> bytes;
		double rate = 0.0;
	};
}

std::vector<playback::DecodedSample> collectMsm5232Samples (const midi::SongIr& ir,
															uint32_t)
{
	SampleCollector collector;

	int groupControl[2] = { 0, 0 };
	int key[kChannels] = { 0 };
	bool keyed[kChannels] = { false };
	bool noise[kChannels] = { false };

	// One build per (divider, enable) pair, not one per write: a driver rewrites the same key and
	// the same group control constantly, and this runs at load.
	std::map<uint64_t, std::vector<uint8_t>> shapes;
	std::map<uint64_t, Pending> pending;

	const auto emit = [&] (int channel)
	{
		const int group = (channel >> 2) & 1;
		const int enabled = groupControl[group];

		if (! keyed[channel] || noise[channel] || enabled == 0)
		{
			return;
		}

		const uint64_t shapeKey = ((uint64_t) (chips::msm5232::dividerOf (key[channel]) & 0xFF) << 8)
								  | (uint64_t) (enabled & 0xFF);

		const auto found = shapes.find (shapeKey);

		if (found == shapes.end ())
		{
			std::vector<float> points;

			if (chips::msm5232::buildShape (chips::msm5232::dividerOf (key[channel]), enabled, points) < 2)
			{
				shapes.emplace (shapeKey, std::vector<uint8_t> ());
				return;
			}

			// Levels run -4..+4 (one unit per enabled tap): divided by four so the file keeps the
			// chip's absolute scale - a single-tap square is a quarter as loud as a four-tap
			// staircase, and normalising would hide that.
			std::vector<uint8_t> bytes;
			bytes.reserve (points.size ());

			for (const float point : points)
			{
				bytes.push_back ((uint8_t) (int8_t) (int) std::lround (point / 4.0f * 127.0f));
			}

			shapes.emplace (shapeKey, bytes);
		}

		const std::vector<uint8_t>& bytes = shapes[shapeKey];

		if (bytes.size () < 2)
		{
			return;
		}

		Pending& entry = pending[hashOf (bytes)];
		entry.bytes = bytes;
		entry.rate = hzOf (key[channel]) * (double) bytes.size ();
	};

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::msm5232)
		{
			continue;
		}

		if (write.reg >= 0x08)
		{
			if (write.reg == 0x0C || write.reg == 0x0D)
			{
				groupControl[write.reg & 1] = write.value & 0x0F;

				// The group control does not key a voice, but it does decide whether the voices it
				// serves are audible - so a key that came first still needs its shape filed.
				for (int i = 0; i < 4; ++i)
				{
					emit ((write.reg & 1) * 4 + i);
				}
			}

			continue;
		}

		const int channel = write.reg & 0x07;
		keyed[channel] = (write.value & 0x80) != 0;
		noise[channel] = write.value >= kNoiseThreshold;

		if (! noise[channel])
		{
			key[channel] = write.value & 0x7F;

			if (key[channel] > chips::msm5232::kLastIndex)
			{
				key[channel] = chips::msm5232::kLastIndex;
			}
		}

		emit (channel);
	}

	for (auto& [hash, entry] : pending)
	{
		if (entry.bytes.size () < 2 || entry.rate <= 0.0)
		{
			continue;
		}

		char name[32] = {};
		std::snprintf (name, sizeof (name), "msm5232_%08X%08X", (unsigned) (hash >> 32),
					   (unsigned) (hash & 0xFFFFFFFFull));

		collector.add (hash, name, entry.bytes.data (), entry.bytes.size (),
					   SampleFormat::pcm8Signed, entry.rate, 0, /*isCycle*/ true);
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
