#include "K054539Samples.h"

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
	constexpr int kVoices = 8;

	/** Each voice owns this many registers, and `0x00-0x1F` is voice 0's block. */
	constexpr uint16_t kChannelBlockSize = 0x20;
	constexpr uint16_t kChannelRegisterMask = 0x1F;

	/** The K054539's sample ROM, as VGM calls it (`K054539Decoder::kRequirements`). */
	constexpr uint8_t kRomType = 0x8C;

	/** Twenty-four address bits, and the ceiling this needs. */
	constexpr std::size_t kMaxRomBytes = 16u << 20;

	constexpr double kFallbackClock = 18432000.0;

	/** `pitch / 65536` units per output sample at `clock / 384` samples a second. */
	constexpr double kStepDivisor = 65536.0 * 384.0;

	/** The three formats, as the type byte's bits 2-3 spell them. */
	constexpr uint8_t kTypeMask = 0x0C;
	constexpr uint8_t kTypeEightBit = 0x00;
	constexpr uint8_t kTypeSixteenBit = 0x04;
	constexpr uint8_t kTypeFourBit = 0x08;

	/** The unit that ends a sample, in each format. */
	constexpr uint8_t kEndMarkerEightBit = 0x80;
	constexpr uint16_t kEndMarkerSixteenBit = 0x8000;
	constexpr uint8_t kEndMarkerFourBit = 0x88;

	/** Far enough for any sample these boards hold, and short enough to bound a malformed one. */
	constexpr std::size_t kMaxMarkerScan = 1u << 18;

	struct Voice
	{
		uint32_t pitch = 0;
		uint32_t start = 0;
		uint8_t type = kTypeEightBit;

		/** Set while the key-on mask has this voice's bit, so one press is one sample. */
		bool keyed = false;
	};

	/** One voice starting one sample, before de-duplication. */
	struct Hit
	{
		uint32_t start = 0;
		std::size_t units = 0;
		uint8_t type = kTypeEightBit;
		uint32_t pitch = 0;
	};

	bool isSixteenBit (uint8_t type) { return type == kTypeSixteenBit; }
	bool isFourBit (uint8_t type) { return type == kTypeFourBit; }

	/** How many bytes one scan step covers. Four-bit is one byte for two nibbles, which is why the scan walks bytes in every format and `units` counts them the same way. */
	std::size_t bytesPerUnit (uint8_t type)
	{
		return isSixteenBit (type) ? 2 : 1;
	}

	/** What the collector needs to know to turn this voice's bytes into levels.

		Three formats, three decoders, and no default that could pass for one of them: a voice whose
		type byte says nothing recognisable is read as eight-bit, which is what the chip does with an
		undefined value too.
	*/
	SampleFormat sampleFormatFor (uint8_t type)
	{
		if (isSixteenBit (type))
		{
			return SampleFormat::pcm16Signed;
		}

		return isFourBit (type) ? SampleFormat::dpcm4 : SampleFormat::pcm8Signed;
	}
}

std::vector<playback::DecodedSample> collectK054539Samples (const midi::SongIr& ir)
{
	SampleCollector collector;

	const std::vector<uint8_t> rom = memory::rom (ir, kRomType, kMaxRomBytes, false);

	if (rom.empty ())
	{
		return collector.take ();
	}

	double clock = kFallbackClock;

	for (const auto& slot : ir.chips)
	{
		if (slot.kind == midi::ChipKind::k054539 && slot.clockHz != 0)
		{
			clock = (double) slot.clockHz;
		}
	}

	Voice voices[kVoices];

	// The marker scan, over bytes rather than through a decoder: this file has no `fetchUnit`, and
	// adding one would be a second copy of the decoder's format arithmetic. What it needs is only
	// where a sample ends.
	const auto markerAt = [&rom] (uint32_t address, uint8_t type) -> bool
	{
		if (isSixteenBit (type))
		{
			if (address + 1 >= rom.size ())
			{
				return false;
			}

			const uint16_t word = (uint16_t) ((uint16_t) rom[address]
										   | ((uint16_t) rom[address + 1] << 8));
			return word == kEndMarkerSixteenBit;
		}

		if (isFourBit (type))
		{
			return (address < rom.size ()) && rom[address] == kEndMarkerFourBit;
		}

		return (address < rom.size ()) && rom[address] == kEndMarkerEightBit;
	};

	/** How many units a sample holds before its marker. */
	const auto unitsOf = [&] (uint32_t start, uint8_t type) -> std::size_t
	{
		const std::size_t stride = bytesPerUnit (type);

		for (std::size_t i = 0; i < kMaxMarkerScan; ++i)
		{
			const uint32_t address = start + (uint32_t) (i * stride);

			if (address >= rom.size ())
			{
				return 0;
			}

			if (markerAt (address, type))
			{
				return i;
			}
		}

		// No marker in reach: the song is malformed or the marker is past the ROM. Reporting the
		// whole scan as the length would invent a sample the size of the search.
		return 0;
	};

	std::vector<Hit> hits;

	const auto collect = [&] (const Voice& voice)
	{
		if (voice.pitch == 0)
		{
			return;
		}

		const std::size_t units = unitsOf (voice.start, voice.type);

		if (units < 2)
		{
			return;
		}

		hits.push_back (Hit { voice.start, units, voice.type, voice.pitch });
	};

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::k054539)
		{
			continue;
		}

		// Sixteen-bit offsets: the high byte travels in the port, as the C352's does.
		const uint16_t offset = (uint16_t) (((uint16_t) write.port << 8) | write.reg);
		const uint8_t value = (uint8_t) write.value;

		// Key-on is a mask over all eight voices, and only a rising edge starts a note.
		if (offset == 0x214)
		{
			for (int i = 0; i < kVoices; ++i)
			{
				const bool on = (value & (1u << i)) != 0;

				if (on && ! voices[i].keyed)
				{
					collect (voices[i]);
				}

				voices[i].keyed = on;
			}

			continue;
		}

		// Key-off is the other mask; a voice that was released is not a new note either way.
		if (offset == 0x215)
		{
			for (int i = 0; i < kVoices; ++i)
			{
				if ((value & (1u << i)) != 0)
				{
					voices[i].keyed = false;
				}
			}

			continue;
		}

		// The type and loop bytes are a second block, one byte apart per voice.
		if (offset >= 0x200 && offset < 0x210)
		{
			const int index = (int) ((offset - 0x200) / 2);

			if (index < kVoices && (offset & 1) == 0)
			{
				voices[index].type = (uint8_t) (value & kTypeMask);
			}

			continue;
		}

		// Everything below 0x200 is a voice block of this many registers.
		if (offset >= kVoices * kChannelBlockSize)
		{
			continue;
		}

		const int index = (int) (offset / kChannelBlockSize);

		if (index < 0 || index >= kVoices)
		{
			continue;
		}

		Voice& voice = voices[index];

		switch (offset & kChannelRegisterMask)
		{
		case 0x00: voice.pitch = (voice.pitch & 0xFFFF00u) | value; break;
		case 0x01: voice.pitch = (voice.pitch & 0xFF00FFu) | ((uint32_t) value << 8); break;
		case 0x02: voice.pitch = (voice.pitch & 0x00FFFFu) | ((uint32_t) value << 16); break;

		// The start registers, which the core also writes the live position back into. What the
		// stream carries here is the address the driver just set, so it is where the sample begins.
		case 0x0C: voice.start = (voice.start & 0xFFFF00u) | value; break;
		case 0x0D: voice.start = (voice.start & 0xFF00FFu) | ((uint32_t) value << 8); break;
		case 0x0E: voice.start = (voice.start & 0x00FFFFu) | ((uint32_t) value << 16); break;
		default: break;
		}
	}

	std::map<uint64_t, uint32_t> highestPitch;

	for (const auto& hit : hits)
	{
		const uint64_t key = ((uint64_t) hit.start << 32) | ((uint64_t) hit.units << 8) | hit.type;
		auto& highest = highestPitch[key];

		highest = (std::max) (highest, hit.pitch);
	}

	for (const auto& hit : hits)
	{
		const uint64_t key = ((uint64_t) hit.start << 32) | ((uint64_t) hit.units << 8) | hit.type;
		const uint32_t pitch = highestPitch[key];
		const double rate = (double) pitch * clock / kStepDivisor;
		// The scan walks bytes in every format - two nibbles share one for four-bit - so `units` is
		// already a byte count for it and `bytesPerUnit` is 1. Dividing by two here would export
		// half of a four-bit sample, which is a waveform that ends early rather than one that looks
		// wrong.
		const std::size_t bytes = hit.units * bytesPerUnit (hit.type);

		if (hit.start >= rom.size () || bytes > rom.size () - hit.start)
		{
			continue;
		}

		char name[32] = {};
		std::snprintf (name, sizeof (name), "k054539_%06X_%u", (unsigned) hit.start,
					   (unsigned) bytes);

		collector.add (key, name, rom.data () + hit.start, bytes,
					   sampleFormatFor (hit.type),
					   rate);
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
