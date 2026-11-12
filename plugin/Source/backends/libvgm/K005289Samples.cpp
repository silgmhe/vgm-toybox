#include "K005289Samples.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

#include "ChipMemoryAssembly.h"
#include "SampleCollector.h"
#include "chips/K005289Decoder.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	constexpr uint32_t kDefaultClock = 3579545;
	constexpr int kPromBytes = 0x200;
	constexpr int kHalfBytes = 0x100;
	constexpr int kWaveformBytes = 32;
	constexpr int kWaveforms = 8;
	constexpr int kChannels = 2;

	/** Four bits with `- 8` on the way out (`k005289.c:198`). */
	constexpr int kWaveBias = 8;

	double stepRateOf (uint32_t clock, uint16_t divider)
	{
		return (double) clock / ((double) divider + 1.0);
	}

	uint64_t hashOf (const uint8_t* bytes)
	{
		uint64_t hash = 1469598103934665603ull;

		for (int i = 0; i < kWaveformBytes; ++i)
		{
			hash ^= (uint64_t) bytes[i];
			hash *= 1099511628211ull;
		}

		return hash;
	}

	struct Chip
	{
		uint16_t latch[kChannels] = { 0, 0 };
		uint16_t freq[kChannels] = { 0, 0 };
		int waveform[kChannels] = { 0, 0 };
	};

	struct Pending
	{
		std::vector<uint8_t> bytes;
		double rate = 0.0;
	};
}

std::vector<playback::DecodedSample> collectK005289Samples (const midi::SongIr& ir,
															uint32_t clockHz)
{
	SampleCollector collector;

	// One image for the whole song: a `0xC3` block is a write at a sixteen-bit address, and the
	// drivers here upload it before the first register write.
	//
	// **The payload is already one byte per entry.** Measured block header: `type=0xC3 len=514`
	// (two address bytes, then 512 of data), then `00 00 | 07 00 07 0A 0C 0D ...` - address zero,
	// then values in 0..15. A reader that unpacked nibbles would halve a table that is not packed.
	const std::vector<uint8_t> prom = memory::ram (ir, chips::K005289Decoder::kRequirements.memoryType,
												  (std::size_t) kPromBytes);

	if (prom.size () < (std::size_t) kPromBytes)
	{
		return {};
	}

	uint32_t clock = clockHz;

	if (clock == 0)
	{
		for (const auto& slot : ir.chips)
		{
			if (slot.kind == midi::ChipKind::k005289 && slot.clockHz != 0)
			{
				clock = slot.clockHz;
			}
		}
	}

	if (clock == 0)
	{
		clock = kDefaultClock;
	}

	std::map<uint32_t, Chip> chips;
	std::map<uint64_t, Pending> pending;

	/** Files the waveform a voice points at. Identity is the thirty-two bytes themselves: the same
		shape used at five pitches is one file, and two channels that happen to share a waveform are
		one too - which is what the PROM says they are. */
	const auto emit = [&] (Chip& chip, int channel)
	{
		const int waveform = chip.waveform[channel];

		if (channel < 0 || channel >= kChannels || waveform < 0 || waveform >= kWaveforms)
		{
			return;
		}

		const double rate = stepRateOf (clock, chip.freq[channel]);

		if (rate <= 0.0)
		{
			return;
		}

		const uint8_t* bytes = prom.data () + channel * kHalfBytes + waveform * kWaveformBytes;
		const uint64_t key = hashOf (bytes);
		Pending& entry = pending[key];

		if (entry.bytes.empty())
		{
			entry.bytes.assign (bytes, bytes + kWaveformBytes);
		}

		entry.rate = rate;
	};

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::k005289)
		{
			continue;
		}

		Chip& chip = chips[write.deviceId];
		const int channel = write.reg & 1;
		const uint16_t data = (uint16_t) (write.value & 0x0FFF);

		switch (write.reg & 0x06)
		{
		case 0x00:		// control: volume in the low nibble, waveform in bits 5-7
			chip.waveform[channel] = (data >> 5) & 0x07;
			break;

		case 0x02:		// LD: latch the inverted divider, as the core does (`k005289.c:224`)
			chip.latch[channel] = (uint16_t) (0x0FFF - data);
			break;

		case 0x04:		// TG: the trigger - the pitch becomes audible and the shape matters
			chip.freq[channel] = chip.latch[channel];

			if (chip.freq[channel] > 0)
			{
				emit (chip, channel);
			}

			break;

		default:
			break;
		}
	}

	for (auto& [key, entry] : pending)
	{
		if (entry.bytes.size () != (std::size_t) kWaveformBytes || entry.rate <= 0.0)
		{
			continue;
		}

		char name[32] = {};
		std::snprintf (name, sizeof (name), "k005289_%08X%08X", (unsigned) (key >> 32),
					   (unsigned) (key & 0xFFFFFFFFull));

		// Sixteen levels: the PROM is four bits, so the data is on a sixteen-step grid.
		//
		// The export's "chip's own depth" will still quantise at six (`nativeBits` is keyed by
		// `waveform::Kind`, and every stored table reads six - the FDS's width). That is a label
		// one step too wide rather than a change to the file: a value already on a sixteen-step
		// grid is unchanged by a sixty-three-step snap. Making it exact needs a per-channel native
		// depth rather than one per kind, which is worth doing when something depends on the label.
		collector.add (key, name, entry.bytes.data (), entry.bytes.size (),
					   SampleFormat::prom4, entry.rate, 0, /*isCycle*/ true);
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
