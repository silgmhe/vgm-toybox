#include "Rf5c68Samples.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <utility>

#include "ChipMemoryAssembly.h"
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

	/** Bytes from `start` to the terminating 0xFF - the only length this chip has. */
	const auto lengthOf = [&] (uint32_t start) -> std::size_t
	{
		std::size_t length = 0;

		while (length < kMaxSampleScan && byteAt (start + (uint32_t) length) != 0xFF)
		{
			++length;
		}

		return (length >= kMaxSampleScan) ? 0 : length;
	};

	std::vector<Hit> hits;

	const auto collect = [&] (const Voice& voice)
	{
		// `0x06` counts 256-byte units, so the address is the register shifted up.
		const uint32_t address = (uint32_t) voice.start << 8;

		if (address >= ram.size () || voice.step == 0)
		{
			return;
		}

		// A sample that wraps past the top cannot be cut as one contiguous run, so the export is
		// what the buffer actually holds from here on. On this chip that is the usual case rather
		// than an edge one: the terminator is the padding at address 0.
		const std::size_t length = (std::min) (lengthOf (address), ram.size () - address);

		if (length < 2)
		{
			return;
		}

		hits.push_back (Hit { address, length, voice.step });
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

	std::map<uint64_t, uint16_t> highestStep;

	for (const auto& hit : hits)
	{
		const uint64_t key = ((uint64_t) hit.address << 32) | hit.length;
		auto& highest = highestStep[key];
		highest = (std::max) (highest, hit.step);
	}

	for (const auto& hit : hits)
	{
		const uint64_t key = ((uint64_t) hit.address << 32) | hit.length;

		// `step * (clock/384) / 2048` bytes a second - the same arithmetic the decoder's
		// `bytesPerSecond` does, and a drum retuned per hit takes the highest of them.
		const double rate = (double) highestStep[key] * clock / kOutputDivisor / kStepPerByte;

		char name[32] = {};
		std::snprintf (name, sizeof (name), "rf5c_%06X_%u", (unsigned) hit.address,
					   (unsigned) hit.length);

		collector.add (key, name, ram.data () + hit.address, hit.length,
					   SampleFormat::pcm8Signed, rate);
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
