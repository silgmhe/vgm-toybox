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
		bool keyed = false;
		uint32_t start = 0;			// assembled from the high, middle and low registers
		uint32_t stop = 0;
		uint32_t low = std::numeric_limits<uint32_t>::max ();		// after the global key-on enable
		uint32_t high = 0;
		uint32_t lowAll = std::numeric_limits<uint32_t>::max ();	// every address, setup included
		uint32_t highAll = 0;
	};

	std::map<uint32_t, Voice> voices;

	/** `0xFF` bit 7: the global key-on enable, and the line between setup and playback. */
	bool enabled = false;

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::ymz280b)
		{
			continue;
		}

		if (write.reg == 0xFF)
		{
			// The global key-on enable: everything written before it is setup, not playback.
			enabled = (write.value & 0x80) != 0;
			continue;
		}

		if (write.reg >= 0x80)
		{
			continue;		// DSP and ROM readback carry no addresses
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
			voice.keyed = (write.value & 0x80) != 0;
			break;

		case 0x02:
			voice.level = write.value;
			break;

		// **The address arrives in three writes and only the last one completes it.** Taking the
		// high byte alone (the first version) started the export up to 64 KB before the real data -
		// and when that lands outside the ROM block the decode is zeros, so the file came out blank
		// while the row (which assembles all three) was fine. `06 Sonic Diver` was that case.
		case 0x20: case 0x23:
			if ((write.reg & 0xE3) == 0x20)
			{
				voice.start = (voice.start & 0x00FFFF) | ((uint32_t) write.value << 16);
			}
			else
			{
				voice.stop = (voice.stop & 0x00FFFF) | ((uint32_t) write.value << 16);
			}

			break;

		case 0x40: case 0x43:
			if ((write.reg & 0xE3) == 0x40)
			{
				voice.start = (voice.start & 0xFF00FF) | ((uint32_t) write.value << 8);
			}
			else
			{
				voice.stop = (voice.stop & 0xFF00FF) | ((uint32_t) write.value << 8);
			}

			break;

		case 0x60: case 0x63:
			if ((write.reg & 0xE3) == 0x60)
			{
				voice.start = (voice.start & 0xFFFF00) | (uint32_t) write.value;

				// **Only ranges written after the global key-on enable count.** These drivers write
				// a start address during setup that points into a silent part of the ROM (measured
				// on `03 Skull Fang`: start 0x394B61 reads 0x00, while the section that plays is
				// 0x3BF6DA). The enable is `0xFF` bit 7 (`ymz280b.c:993`), and it is written before
				// the addresses that matter - gating on the voice's own key-on instead was too
				// strict, because the addresses are written *before* that.
				voice.lowAll = std::min (voice.lowAll, voice.start);

				if (enabled)
				{
					voice.low = std::min (voice.low, voice.start);
				}
			}
			else
			{
				voice.stop = (voice.stop & 0xFFFF00) | (uint32_t) write.value;

				voice.highAll = std::max (voice.highAll, voice.stop);

				if (enabled)
				{
					voice.high = std::max (voice.high, voice.stop);
				}
			}

			break;

		default:
			break;
		}
	}

	// **One file: the whole block, decoded once.**

	// These rips stream a song out of a single ROM block, and slicing that block by the addresses a
	// driver happened to write is what produced DC: the union of those ranges can begin in a part of
	// the image the song never plays (measured on `06 Sonic Diver`), and on `02 Spirit of Fang` it
	// produced no range at all. The block **is** the content - the drivers just walk it - so decode
	// all of it at the rate of the voice that reads it, and let the file be the track.
	std::size_t first = rom.size ();
	std::size_t last = 0;

	for (std::size_t i = 0; i < rom.size (); ++i)
	{
		if (rom[i] != 0)
		{
			if (first == rom.size ())
			{
				first = i;
			}

			last = i;
		}
	}

	if (first >= rom.size () || last <= first)
	{
		return collector.take ();
	}

	// The loudest voice sets the rate: every song here uses one voice, and a second file per rate
	// would only duplicate the same bytes.
	const Voice* widest = nullptr;

	for (const auto& entry : voices)
	{
		if (entry.second.level > 0 && (widest == nullptr || entry.second.level > widest->level))
		{
			widest = &entry.second;
		}
	}

	if (widest == nullptr)
	{
		return collector.take ();
	}

	const uint32_t begin = (uint32_t) first;
	const uint32_t span = (uint32_t) (last + 1 - first);
	const uint32_t steps = std::min (span * ((widest->mode == 1) ? 2u : 1u), kMaxSteps);
	std::vector<float> points;

	chips::ymz280b::decodeRange (rom, widest->mode, begin, steps, points);

	if (points.size () < 2)
	{
		return collector.take ();
	}

	// One signed byte per level, the shape the collector's format expects.
	std::vector<uint8_t> bytes;
	bytes.reserve (points.size ());

	for (const float point : points)
	{
		bytes.push_back ((uint8_t) (int8_t) (int) std::lround (point * 127.0f));
	}

	collector.add (hashOf (bytes), "ymz280b_stream", bytes.data (), bytes.size (),
				   SampleFormat::pcm8Signed,
				   chips::ymz280b::rateOf (clock, widest->fnum, widest->mode));

	return collector.take ();
}

} // namespace toybox::backends::libvgm
