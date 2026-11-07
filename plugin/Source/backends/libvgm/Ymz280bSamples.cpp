#include "Ymz280bSamples.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <vector>

#include "ChipMemoryAssembly.h"
#include "SampleCollector.h"
#include "chips/Ymz280bData.h"
#include "chips/Ymz280bDecoder.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	constexpr uint32_t kDefaultClock = 16934400;
	constexpr int kChannels = 8;

	/** A cap on what one voice may export. The streamed rips here name ranges in the tens of
		thousands of nibbles; this only stops a broken address from asking for megabytes. */
	constexpr uint32_t kMaxSteps = 1u << 21;

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
}

std::vector<playback::DecodedSample> collectYmz280bSamples (const midi::SongIr& ir,
															uint32_t clockHz)
{
	SampleCollector collector;

	const std::vector<uint8_t> rom = memory::rom (ir, chips::Ymz280bDecoder::kRequirements.memoryType,
												  chips::Ymz280bDecoder::kRequirements.memoryBytes, false);

	if (rom.empty ())
	{
		return {};
	}

	uint32_t clock = clockHz;

	if (clock == 0)
	{
		for (const auto& slot : ir.chips)
		{
			if (slot.kind == midi::ChipKind::ymz280b && slot.clockHz != 0)
			{
				clock = slot.clockHz;
			}
		}
	}

	if (clock == 0)
	{
		clock = kDefaultClock;
	}

	struct Voice
	{
		int fnum = 0;
		int mode = 0;
		int level = 0;
		uint32_t low = std::numeric_limits<uint32_t>::max ();
		uint32_t high = 0;
	};

	std::map<uint32_t, Voice> voices;

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::ymz280b)
		{
			continue;
		}

		if (write.reg >= 0x80)
		{
			continue;		// DSP, ROM readback and the global key-on enable: no addresses here
		}

		const int index = (write.reg >> 2) & 7;
		Voice& voice = voices[(uint32_t) (write.deviceId * 8 + index)];

		switch (write.reg & 0xE3)
		{
		case 0x00:
			voice.fnum = (voice.fnum & 0x100) | write.value;
			break;

		case 0x01:
			voice.fnum = (voice.fnum & 0xFF) | ((write.value & 0x01) << 8);
			voice.mode = (write.value & 0x60) >> 5;
			break;

		case 0x02:
			voice.level = write.value;
			break;

		case 0x20: case 0x21: case 0x22: case 0x23:
		{
			// Start and stop are what bound the recording; loop start and loop end move with them
			// while a stream is fed, so the union over the song is the honest range.
			const int which = (write.reg & 0xE3) - 0x20;
			const uint32_t value = (uint32_t) write.value << 16;

			if (which == 0 || which == 1)
			{
				if (voice.low == std::numeric_limits<uint32_t>::max ())
				{
					voice.low = value;		// the high byte arrives first, so it opens the address
				}
			}
			else if (which == 3)
			{
				voice.high = std::max (voice.high, value);
			}

			break;
		}

		case 0x43:
			voice.high = std::max (voice.high, (uint32_t) write.value << 8);
			break;

		case 0x63:
			voice.high = std::max (voice.high, (uint32_t) write.value);
			break;

		default:
			break;
		}
	}

	for (auto& [key, voice] : voices)
	{
		if (voice.level == 0 || voice.high <= voice.low)
		{
			continue;
		}

		const uint32_t steps = std::min (voice.high - voice.low, kMaxSteps);
		std::vector<float> points;

		chips::ymz280b::decodeRange (rom, voice.mode, voice.low, steps, points);

		if (points.size () < 2)
		{
			continue;
		}

		// One signed byte per level, the shape the collector's format expects.
		std::vector<uint8_t> bytes;
		bytes.reserve (points.size ());

		for (const float point : points)
		{
			bytes.push_back ((uint8_t) (int8_t) (int) std::lround (point * 127.0f));
		}

		const uint64_t hash = hashOf (bytes);
		char name[32] = {};
		std::snprintf (name, sizeof (name), "ymz280b_v%u", (unsigned) (key % 8));

		collector.add (hash, name, bytes.data (), bytes.size (), SampleFormat::pcm8Signed,
					   chips::ymz280b::rateOf (clock, voice.fnum, voice.mode));
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
