#include "Y8950Samples.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <tuple>
#include <vector>

#include "ChipMemoryAssembly.h"
#include "SampleCollector.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	/** The clock a song with no header value is measured against: the 3.58 MHz part the Y8950 was
		designed around, and the same fallback `OplDecoder::kRequirements` names. */
	constexpr double kDefaultClock = 3579545.0;

	/** The unit addresses pages of 32 bytes (or 4, for a DRAM part), so its 16-bit registers reach
		`0x10000 << 5` = 2 MB. The image is built to the blocks' declared size, well inside that. */
	constexpr std::size_t kMaxImageBytes = 1u << 24;

	/** A range longer than this at its own rate is a bank, not a sample: `stop` can be pointed at
		the end of the ROM, and a minute-long file cannot be what the driver meant. */
	constexpr double kMaxSampleSeconds = 30.0;

	/** Delta-T's address shift, from `ymdeltat.c`: a page is 32 bytes, and control2 whose low two
		bits are 0 selects a part with four-byte granularity. */
	int shiftFor (uint8_t mode)
	{
		static constexpr int kDramRightShift[4] = { 3, 0, 0, 0 };
		return 5 - kDramRightShift[mode & 3];
	}
}

std::vector<playback::DecodedSample> collectY8950Samples (const midi::SongIr& ir, uint32_t clockHz)
{
	SampleCollector collector;

	std::size_t devices = 0;

	for (const auto& slot : ir.chips)
	{
		if (slot.kind == midi::ChipKind::y8950)
		{
			++devices;
		}
	}

	if (devices == 0)
	{
		return collector.take ();
	}

	// **One image for the whole song.** `0x88` blocks place their payload at the ROM offset they
	// declare, so the three blocks of `05 Impact 151` land at 0, 0x4000 and 0x7000 of a 32 KB image.
	const std::vector<uint8_t> rom = memory::rom (ir, 0x88, kMaxImageBytes, false);

	if (rom.empty ())
	{
		return collector.take ();
	}

	for (std::size_t index = 0; index < ir.chips.size (); ++index)
	{
		if (ir.chips[index].kind != midi::ChipKind::y8950)
		{
			continue;
		}

		const double clock = ir.chips[index].clockHz > 0
			? (double) ir.chips[index].clockHz
			: (clockHz > 0 ? (double) clockHz : kDefaultClock);

		uint8_t mode = 0;
		uint8_t startLow = 0;
		uint8_t startHigh = 0;
		uint8_t stopLow = 0;
		uint8_t stopHigh = 0;
		uint16_t delta = 0;

		// The same range is started dozens of times; the registers do not change between hits.
		std::map<std::tuple<uint32_t, uint32_t, uint32_t>, bool> visited;

		// **CPU-fed mode** (control 0x07 bit 5 clear): there is no ROM range, so the sample is the
		// bytes the driver writes to 0x0F between START and the next control write. They are
		// captured here and emitted as one file, named by content because they have no address.
		std::vector<uint8_t> cpuBytes;
		bool capturing = false;

		const auto emitCpu = [&] (void)
		{
			if (cpuBytes.size () < 2 || delta == 0)
			{
				cpuBytes.clear ();
				return;
			}

			const double rate = (double) delta * clock / (72.0 * 65536.0);

			if (rate <= 0.0)
			{
				cpuBytes.clear ();
				return;
			}

			uint64_t hash = 1469598103934665603ull;
			hash = (hash ^ (uint64_t) index) * 1099511628211ull;

			for (const uint8_t byte : cpuBytes)
			{
				hash = (hash ^ (uint64_t) byte) * 1099511628211ull;
			}

			hash ^= (uint64_t) cpuBytes.size () << 40;

			char name[48] = {};

			if (devices > 1)
			{
				std::snprintf (name, sizeof (name), "y8950adpcm_cpu_%08X%08X_d%u",
							   (unsigned) (hash >> 32), (unsigned) (hash & 0xFFFFFFFFull),
							   (unsigned) index);
			}
			else
			{
				std::snprintf (name, sizeof (name), "y8950adpcm_cpu_%08X%08X", (unsigned) (hash >> 32),
							   (unsigned) (hash & 0xFFFFFFFFull));
			}

			collector.add (hash, name, cpuBytes.data (), cpuBytes.size (), SampleFormat::adpcmB4,
						   rate);

			cpuBytes.clear ();
		};

		for (const auto& write : ir.writes)
		{
			if ((std::size_t) write.deviceId != index || write.port != 0)
			{
				continue;
			}

			switch (write.reg)
			{
			case 0x08: mode = write.value; break;
			case 0x09: startLow = write.value; break;
			case 0x0A: startHigh = write.value; break;
			case 0x0B: stopLow = write.value; break;
			case 0x0C: stopHigh = write.value; break;

			case 0x0F:				// the ADPCM data port, used only in CPU-fed mode
				if (capturing && cpuBytes.size () < (1u << 20))
				{
					cpuBytes.push_back ((uint8_t) write.value);
				}

				break;

			case 0x10:
				delta = (uint16_t) ((delta & 0xFF00u) | write.value);
				break;

			case 0x11:
				delta = (uint16_t) ((delta & 0x00FFu) | ((uint16_t) write.value << 8));
				break;

			case 0x07:
			{
				const bool startBit = (write.value & 0x80u) != 0;
				const bool externalMemory = (write.value & 0x20u) != 0;

				const bool wasCapturing = capturing;
				capturing = startBit && ! externalMemory;

				if (wasCapturing && ! capturing)
				{
					emitCpu ();		// START cleared, or the mode changed under us
				}

				if (capturing && ! wasCapturing)
				{
					cpuBytes.clear ();	// a new stream, not the previous hit's tail
				}

				if (capturing || ! startBit || delta == 0)
				{
					break;			// CPU-fed: nothing to find in the image; 0x0F fills it
				}

				const int shift = shiftFor (mode);
				const uint32_t start = (uint32_t) ((startHigh << 8) | startLow) << shift;
				const uint32_t last = (((uint32_t) ((stopHigh << 8) | stopLow)) << shift)
									  | ((1u << shift) - 1u);

				if (last < start || start >= rom.size ())
				{
					break;
				}

				// **`last` is the byte the unit stops on**, so the range includes it - the same rule
				// the OPN's Delta-T exporter follows.
				const std::size_t length =
					(std::min<std::size_t>) (last - start + 1u, rom.size () - start);

				if (length < 2)
				{
					break;
				}

				const double rate = (double) delta * clock / (72.0 * 65536.0);

				if (rate <= 0.0 || 2.0 * (double) length / rate > kMaxSampleSeconds)
				{
					break;
				}

				if (! visited.emplace (std::make_tuple (start, (uint32_t) length, (uint32_t) delta),
									   true).second)
				{
					break;
				}

				uint64_t hash = 1469598103934665603ull;
				hash = (hash ^ (uint64_t) index) * 1099511628211ull;

				for (std::size_t i = 0; i < length; ++i)
				{
					hash = (hash ^ rom[start + i]) * 1099511628211ull;
				}

				hash ^= (uint64_t) length << 40;

				char name[48] = {};

				if (devices > 1)
				{
					std::snprintf (name, sizeof (name), "y8950adpcm_%06X_%05X_d%u", (unsigned) start,
								   (unsigned) length, (unsigned) index);
				}
				else
				{
					std::snprintf (name, sizeof (name), "y8950adpcm_%06X_%05X", (unsigned) start,
								   (unsigned) length);
				}

				collector.add (hash, name, rom.data () + start, length, SampleFormat::adpcmB4,
							   rate);
				break;
			}

			default:
				break;
			}
		}

		// A stream still open at the end of the song is a sample too: the driver's last hit may
		// never be followed by a control write.
		if (capturing)
		{
			emitCpu ();
		}
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
