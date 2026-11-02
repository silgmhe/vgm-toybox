#include "Rf5c68Samples.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <utility>

#include "ChipMemoryAssembly.h"
#include "chips/Rf5c68Hit.h"
#include "SampleCollector.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	constexpr int kChannels = 8;

	/** The RAM the chip addresses, sixteen bits wide - the same ceiling the decoder uses. */
	constexpr std::size_t kRamBytes = 64 * 1024;

	/** A PCM bank in VGM: blocks below 0x40 concatenate, no header. */
	constexpr uint8_t kPcmBankType = 0x02;

	constexpr double kFallbackClock = 12500000.0;

	/** The chip runs at `clock / 384` samples a second. */
	constexpr double kOutputDivisor = 384.0;

	/** A step of 2048 advances one byte per chip sample. */
	constexpr double kStepPerByte = 2048.0;

	/** `0xFF` ends a sample; one that never reaches one is not countable. */
	constexpr uint32_t kMaxSampleScan = kRamBytes;

	struct Voice
	{
		uint8_t start = 0;			// 0x06, in 256-byte units
		uint16_t step = 0;			// 0x02 / 0x03, the playback rate and the only pitch
		bool enabled = false;
	};

	/** One voice starting one sample, before de-duplication. */
	struct Hit
	{
		uint32_t address = 0;
		std::size_t length = 0;
		uint16_t step = 0;
	};
}

std::vector<playback::DecodedSample> collectRf5cSamples (const midi::SongIr& ir)
{
	SampleCollector collector;

	// A PCM bank, not a ROM block: `0x67` blocks of type 0x02, concatenated in stream order. The
	// runtime hands the decoder the same image through `setRam`.
	const std::vector<uint8_t> ram = memory::pcmBank (ir, kPcmBankType, kRamBytes);

	if (ram.empty ())
	{
		return collector.take ();
	}

	double clock = kFallbackClock;

	for (const auto& slot : ir.chips)
	{
		if ((slot.kind == midi::ChipKind::rf5c68 || slot.kind == midi::ChipKind::rf5c164)
			&& slot.clockHz != 0)
		{
			clock = (double) slot.clockHz;
		}
	}

	Voice voices[kChannels];
	uint8_t bank = 0;

	/** The same read the decoder does: sixteen-bit address, wrapping at the top.
	 *
	 * The wrap matters here. Sonic CD's samples have no `0xFF` inside them at all - what stops
	 * them is the padding at address 0, which the core reaches by wrapping past 0xFFFF. Reading
	 * "past the end is the end" instead happens to give the same length on that rip, but only
	 * because the byte at address 0 is `0xFF`; a sample whose loop point is elsewhere would come
	 * out short.
	 */
	const auto byteAt = [&ram] (uint32_t address) -> uint8_t
	{
		const uint32_t index = address & 0xFFFFu;

		return (index < ram.size ()) ? ram[index] : 0xFF;
	};

	std::vector<Hit> hits;

	/** Records that a channel started at an address; the length is settled afterwards.

		Scanning to a `0xFF` is the wrong way to size a sample here, and the first version of this
		file did it. On a Sonic CD track the samples contain no `0xFF` at all - what they reach is
		the padding at address 0, which the hardware arrives at by wrapping past 0xFFFF. So every
		sample scanned to the top of RAM and the export became six files that all ended at 0x10000,
		each one containing the samples that came after it.

		What the layout actually says: the six starts are `0x2000`, `0x4000`, `0x6000`, `0x8000`,
		`0xA000`, `0xE000` - every one of them a multiple of 0x2000, eight kilobytes apart. That is
		the bank's slot size, so a sample runs from its start to the next one. Deciding it after the
		whole stream has been read is the only way to know the neighbour.
	*/
	const auto collect = [&] (const Voice& voice)
	{
		// `0x06` counts 256-byte units, so the address is the register shifted up.
		const uint32_t address = (uint32_t) voice.start << 8;

		if (address >= ram.size () || voice.step == 0)
		{
			return;
		}

		// A start that lands on the 0xFF padding is a channel the driver never gave a sample to.
		// Address 0 of these rips is exactly that, and treating it as one produced a 57 KB file of
		// silence plus a row that could never draw.
		if (ram[address] == 0xFF)
		{
			return;
		}

		hits.push_back (Hit { address, 0, voice.step });
	};

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ())
		{
			continue;
		}

		const midi::ChipKind kind = ir.chips[write.deviceId].kind;

		if (kind != midi::ChipKind::rf5c68 && kind != midi::ChipKind::rf5c164)
		{
			continue;
		}

		const uint8_t offset = write.reg;
		const uint8_t value = (uint8_t) write.value;

		// A cleared bit enables a channel - so enabling is a falling edge in this mask, and it is
		// the only key-on the chip has.
		if (offset == 0x08)
		{
			for (int i = 0; i < kChannels; ++i)
			{
				const bool on = ((~value >> i) & 1) != 0;

				if (on && ! voices[i].enabled)
				{
					collect (voices[i]);
				}

				voices[i].enabled = on;
			}

			continue;
		}

		// Bit 6 selects the channel the low registers address; bit 7 is the output switch.
		if (offset == 0x07)
		{
			if ((value & 0x40) != 0)
			{
				bank = (uint8_t) (value & 0x07);
			}

			continue;
		}

		Voice& voice = voices[bank];

		switch (offset)
		{
		case 0x02: voice.step = (uint16_t) ((voice.step & 0xFF00) | value); break;
		case 0x03: voice.step = (uint16_t) ((voice.step & 0x00FF) | ((uint16_t) value << 8)); break;
		case 0x06: voice.start = value; break;
		default: break;
		}
	}

	// One entry per address. The rate is the **most frequent** step, not the highest: these slots
	// are triggered hundreds of times at many pitches (299 hits on one, steps 619..2503), so any
	// single choice is a transposition, and the one the driver uses most is the least surprising.
	std::map<uint32_t, std::map<uint16_t, int>> histograms;
	std::map<uint32_t, uint16_t> byAddress;

	for (const auto& hit : hits)
	{
		++histograms[hit.address][hit.step];

		uint16_t& chosen = byAddress[hit.address];

		if (chosen == 0)
		{
			chosen = hit.step;
		}
	}

	for (auto& entry : byAddress)
	{
		const auto& histogram = histograms[entry.first];
		int best = 0;

		for (const auto& step : histogram)
		{
			if (step.second > best)
			{
				best = step.second;
				entry.second = step.first;
			}
		}
	}

	for (auto current = byAddress.begin (); current != byAddress.end (); ++current)
	{
		const uint32_t address = current->first;
		const auto next = std::next (current);

		// The region runs to the next sample's start, or to the top of RAM for the last one. This
		// is the bank's slot boundary, not a terminator: see the note above `collect`.
		const uint32_t limit = (next != byAddress.end ()) ? next->first : (uint32_t) ram.size ();
		const std::size_t span = (limit > address) ? (std::size_t) (limit - address) : 0;
		const std::size_t length = chips::rf5c68HitLength (address, span,
														  [&ram] (uint32_t a) { return ram[a]; });

		if (length < 2)
		{
			continue;
		}

		const uint64_t key = ((uint64_t) address << 32) | length;

		// `step * (clock/384) / 2048` bytes a second - the same arithmetic the decoder's
		// `bytesPerSecond` does.
		const double rate = (double) current->second * clock / kOutputDivisor / kStepPerByte;

		char name[32] = {};
		std::snprintf (name, sizeof (name), "rf5c_%06X_%u", (unsigned) address, (unsigned) length);

		collector.add (key, name, ram.data () + address, length, SampleFormat::signMagnitude8, rate);
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
