#include "OpnDeltaTSamples.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

#include "ChipMemoryAssembly.h"
#include "SampleCollector.h"
#include "chips/OpnDecoder.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	/** The clock a song with no header value is measured against - the same fallback the replayer's
		requirement names. */
	constexpr double kDefaultClock = 8000000.0;

	/** The DELTA-N a Delta-T ROM is taken to be recorded at: 0x4000 is the value the sets use for an
		untransposed sample, and the only reference the chip offers (it has no root register). Every
		file is written at this rate; the song's own DELTA-N is a transposition on top of it. */
	constexpr uint16_t kReferenceDelta = 0x4000;

	/** A range longer than this at the reference rate is a bank, not a sample. `07 Stage 2` points
		some `end` registers at the end of an 8 MB image; decoding that into levels costs 128 MB and
		cannot be what the driver meant. */
	constexpr double kMaxSampleSeconds = 30.0;

	struct Voice
	{
		uint16_t start = 0;
		uint16_t end = 0;
		uint16_t delta = 0;
	};

	/** ADPCM-B's register block, as an offset from the board's own base. The OPNB puts it at
		`0x10..0x1F` on port 0, the OPNA at `0x00..0x0F` on port 1 - the same split the decoder's
		`write()` dispatches on. */
	bool deltaTOffset (chips::OpnDecoder::Variant variant, const chips::RegisterWrite& write,
					   int& offset)
	{
		const int port = write.port != 0 ? 1 : 0;
		const int reg = write.reg;

		switch (variant)
		{
		case chips::OpnDecoder::Variant::ym2610:
		case chips::OpnDecoder::Variant::ym2610b:
			if (port == 0 && reg >= 0x10 && reg < 0x20)
			{
				offset = reg - 0x10;
				return true;
			}
			return false;

		case chips::OpnDecoder::Variant::ym2608:
			if (port == 1 && reg < 0x10)
			{
				offset = reg;
				return true;
			}
			return false;

		default:
			return false;
		}
	}

	/** The image for one block type, built once per song. */
	const std::vector<uint8_t>* imageFor (const midi::SongIr& ir, uint8_t type,
										  std::map<uint8_t, std::vector<uint8_t>>& images)
	{
		if (type == 0)
		{
			return nullptr;
		}

		auto found = images.find (type);

		if (found == images.end ())
		{
			found = images.emplace (type, memory::rom (ir, type, chips::OpnDecoder::kAdpcmBImageBytes,
													   false))
						.first;
		}

		return found->second.empty () ? nullptr : &found->second;
	}
}

std::vector<playback::DecodedSample> collectOpnDeltaTSamples (const midi::SongIr& ir,
														  uint32_t clockHz)
{
	SampleCollector collector;
	std::map<uint8_t, std::vector<uint8_t>> images;

	std::size_t opnDevices = 0;

	for (const auto& slot : ir.chips)
	{
		if (slot.kind == midi::ChipKind::ym2610 || slot.kind == midi::ChipKind::ym2608)
		{
			++opnDevices;
		}
	}

	for (std::size_t slotIndex = 0; slotIndex < ir.chips.size (); ++slotIndex)
	{
		const midi::ChipSlot& slot = ir.chips[slotIndex];

		const chips::OpnDecoder::Variant variant =
			slot.kind == midi::ChipKind::ym2610 ? chips::OpnDecoder::Variant::ym2610
			: slot.kind == midi::ChipKind::ym2608 ? chips::OpnDecoder::Variant::ym2608
												  : chips::OpnDecoder::Variant::ym2612;

		if (variant == chips::OpnDecoder::Variant::ym2612)
		{
			continue;			// no ADPCM-B on the OPN2
		}

		// The Delta-T block first, then the OPNA's shared ADPCM-A part - one rule, the decoder's.
		const auto blocks = chips::OpnDecoder::sampleBlocksFor (variant);
		const uint8_t types[2] = { blocks.deltaT, blocks.deltaTFallback };
		const std::vector<uint8_t>* rom = nullptr;

		for (const uint8_t type : types)
		{
			if ((rom = imageFor (ir, type, images)) != nullptr)
			{
				break;
			}
		}

		if (rom == nullptr)
		{
			continue;
		}

		const double clock = slot.clockHz > 0 ? (double) slot.clockHz
			: (clockHz > 0 ? (double) clockHz : kDefaultClock);

		// Nibbles a second at the reference DELTA-N: one nibble is one level, and the rate is the
		// same relation the decoder's `adpcmBUnitsPerSecond()` uses.
		const double rate = (double) kReferenceDelta * clock / (144.0 * 65536.0);

		if (rate <= 0.0)
		{
			continue;
		}

		// A range is identified by its address and length, not by DELTA-N: the same recording played
		// at five pitches is one file, and hashing it once is enough.
		std::map<std::pair<uint32_t, uint32_t>, bool> visited;
		Voice voice;

		for (const auto& write : ir.writes)
		{
			if ((std::size_t) write.deviceId != slotIndex)
			{
				continue;
			}

			int offset = 0;

			if (! deltaTOffset (variant, write, offset))
			{
				continue;
			}

			switch (offset)
			{
			case 0x02:
				voice.start = (uint16_t) ((voice.start & 0xFF00u) | write.value);
				break;

			case 0x03:
				voice.start = (uint16_t) ((voice.start & 0x00FFu) | ((uint16_t) write.value << 8));
				break;

			case 0x04:
				voice.end = (uint16_t) ((voice.end & 0xFF00u) | write.value);
				break;

			case 0x05:
				voice.end = (uint16_t) ((voice.end & 0x00FFu) | ((uint16_t) write.value << 8));
				break;

			case 0x09:
				voice.delta = (uint16_t) ((voice.delta & 0xFF00u) | write.value);
				break;

			case 0x0A:
				voice.delta = (uint16_t) ((voice.delta & 0x00FFu) | ((uint16_t) write.value << 8));
				break;

			case 0x00:
			{
				if ((write.value & 0x80u) == 0)
				{
					break;			// repeat/reset, not a key-on
				}

				// DELTA-N 0 is silence on the chip, not a sample at zero rate.
				if (voice.delta == 0)
				{
					break;
				}

				const int shift = chips::OpnDecoder::addressShiftFor (variant);
				const uint32_t start = (uint32_t) voice.start << shift;
				const uint32_t last = ((uint32_t) voice.end << shift) + ((1u << shift) - 1u);

				if (last < start || start >= rom->size ())
				{
					break;
				}

				// **`last` is the last byte the chip plays**, so the range includes it. Using the
				// difference alone dropped a nibble from every range - invisible on a long sample,
				// and wrong on a one-page one.
				const std::size_t length =
					(std::min<std::size_t>) (last - start + 1u, rom->size () - start);

				if (length < 2)
				{
					break;
				}

				const double seconds = 2.0 * (double) length / rate;

				if (seconds > kMaxSampleSeconds)
				{
					break;
				}

				if (! visited.emplace (std::make_pair (start, (uint32_t) length), true).second)
				{
					break;
				}

				uint64_t hash = 1469598103934665603ull;
				hash = (hash ^ (uint64_t) slotIndex) * 1099511628211ull;

				for (std::size_t i = 0; i < length; ++i)
				{
					hash = (hash ^ rom->at (start + i)) * 1099511628211ull;
				}

				hash ^= (uint64_t) length << 40;

				char name[48] = {};

				if (opnDevices > 1)
				{
					std::snprintf (name, sizeof (name), "opndeltat_%06X_%05X_d%u", (unsigned) start,
								   (unsigned) length, (unsigned) slotIndex);
				}
				else
				{
					std::snprintf (name, sizeof (name), "opndeltat_%06X_%05X", (unsigned) start,
								   (unsigned) length);
				}

				collector.add (hash, name, rom->data () + start, length, SampleFormat::adpcmB4, rate);
				break;
			}

			default:
				break;
			}
		}
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
