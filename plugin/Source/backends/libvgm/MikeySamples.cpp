#include "MikeySamples.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

#include "SampleCollector.h"
#include "chips/MikeyPattern.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	constexpr uint32_t kDefaultClock = 16000000;
	constexpr int kChannels = 4;
	constexpr int kTicksShift = 4;

	double stepRateOf (uint32_t clock, int backup, int shift)
	{
		const double ticks = std::pow (2.0, (double) (shift + kTicksShift));

		return (double) clock / ((double) (backup + 1) * ticks);
	}

	/** The hash is over the pattern's **bytes**, which is what decides a file: two states that
		quantise to the same steps produce the same shape, and a float's last bits are not a
		difference anyone can hear. */

	struct Channel
	{
		int volume = 0;
		int backup = 0;
		int shift = 0;
		bool integrate = false;
		uint16_t shiftReg = 0;
		uint16_t taps = 0;
	};

	struct Pending
	{
		std::vector<uint8_t> bytes;
		double rate = 0.0;
	};
}

std::vector<playback::DecodedSample> collectMikeySamples (const midi::SongIr& ir,
														 uint32_t clockHz)
{
	SampleCollector collector;

	uint32_t clock = clockHz;

	if (clock == 0)
	{
		for (const auto& slot : ir.chips)
		{
			if (slot.kind == midi::ChipKind::mikey && slot.clockHz != 0)
			{
				clock = slot.clockHz;
			}
		}
	}

	if (clock == 0)
	{
		clock = kDefaultClock;
	}

	std::map<uint32_t, Channel> chips;
	std::map<uint64_t, Pending> pending;

	/** **The patterns already built, keyed by the three inputs that decide one.**

		A song writes volume, taps and the seed tens of thousands of times, and a great many of those
		writes leave the pattern unchanged (the same taps rewritten, the same seed reloaded). Without
		this memo the walk rebuilt an LFSR cycle for every one of them, which turned a song that used
		to open instantly into a two-second wait - the collector runs at load, and 20,000 walks of up
		to 576 states each is not a free operation.

		Volume is deliberately not in the key: it scales the output, and the pattern is normalised,
		so a level change alone cannot produce a different shape.
	*/
	std::map<uint64_t, std::vector<uint8_t>> built;

	/** Files the pattern the channel is generating right now.

		A Mikey channel has no key-on edge - it sounds whenever its level is non-zero - so the walk
		files the current pattern after each write that touches one of the inputs. The memo above
		collapses repeats, and the content hash collapses two states that happen to produce the same
		bytes.
	*/
	const auto emit = [&] (const Channel& channel)
	{
		if (channel.volume == 0 || channel.backup <= 0)
		{
			return;
		}

		const uint64_t shapeKey = ((uint64_t) channel.shiftReg << 16)
								  | ((uint64_t) channel.taps << 1)
								  | (channel.integrate ? 1ull : 0ull);

		const auto memo = built.find (shapeKey);

		if (memo == built.end ())
		{
			std::vector<float> points;

			if (! chips::mikey::buildPattern (channel.shiftReg, channel.taps, channel.volume,
											  channel.integrate, points)
				|| points.size () < 2)
			{
				built.emplace (shapeKey, std::vector<uint8_t> ());	// nothing to draw; remember it
				return;
			}

			// One signed byte per step, which is what the pattern is: two levels for a plain tone,
			// more when the integrator turns it into a staircase.
			std::vector<uint8_t> bytes;
			bytes.reserve (points.size ());

			for (const float point : points)
			{
				bytes.push_back ((uint8_t) (int8_t) (int) std::lround (point * 127.0f));
			}

			built.emplace (shapeKey, bytes);
		}

		const std::vector<uint8_t>& bytes = built[shapeKey];

		if (bytes.size () < 2)
		{
			return;
		}

		const double rate = stepRateOf (clock, channel.backup, channel.shift);

		if (rate <= 0.0)
		{
			return;
		}

		Pending& entry = pending[hashOf (bytes)];
		entry.bytes = bytes;
		entry.rate = rate;
	};

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::mikey)
		{
			continue;
		}

		if (write.reg < 0x20 || write.reg >= 0x40)
		{
			continue;		// the stereo attenuators above 0x40 carry no shape
		}

		const int index = (write.reg >> 3) & 0x03;
		Channel& channel = chips[(uint32_t) (write.deviceId * 8 + index)];
		const int value = (int) write.value;

		switch (write.reg & 0x07)
		{
		case 0x0:		// VOLCNTRL, a signed level
			channel.volume = (std::int8_t) (uint8_t) value;
			break;

		case 0x1:		// FEEDBACK, the tap selector
			channel.taps = (uint16_t) ((channel.taps & 0x3C0) | (value & 0x3F)
									   | ((value & 0xC0) << 4));
			break;

		case 0x3:		// SHIFT, the register's low eight bits
			channel.shiftReg = (uint16_t) ((channel.shiftReg & 0x0F00) | (value & 0xFF));
			break;

		case 0x4:		// BACKUP, the timer's reload value
			channel.backup = value & 0xFF;
			break;

		case 0x5:		// CTRLA: clock shift, integrate, one extra tap
			channel.shift = value & 0x07;
			channel.integrate = (value & 0x20) != 0;
			channel.taps = (uint16_t) ((channel.taps & 0xF7F) | (value & 0x80));
			break;

		case 0x7:		// OTHER: control B low nibble, the register's top four bits above
			channel.shiftReg = (uint16_t) ((channel.shiftReg & 0x0FF) | ((value & 0xF0) << 4));
			break;

		default:		// 0x2 OUTPUT and 0x6 COUNTER do not change the pattern's inputs
			continue;
		}

		emit (channel);
	}

	for (auto& [key, entry] : pending)
	{
		if (entry.bytes.size () < 2 || entry.rate <= 0.0)
		{
			continue;
		}

		char name[32] = {};
		std::snprintf (name, sizeof (name), "mikey_%08X%08X", (unsigned) (key >> 32),
					   (unsigned) (key & 0xFFFFFFFFull));

		collector.add (key, name, entry.bytes.data (), entry.bytes.size (),
					   SampleFormat::pcm8Signed, entry.rate, 0, /*isCycle*/ true);
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
