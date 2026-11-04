#include "Okim6295Samples.h"

#include <algorithm>
#include <cstdio>
#include <map>

#include "ChipMemoryAssembly.h"
#include "SampleCollector.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	constexpr int kVoices = 4;

	/** The OKIM6295's ROM, as VGM calls it - the same type its `kRequirements` names, so the
		image assembled here is the one the decoder was handed. */
	constexpr uint8_t kRomType = 0x8B;

	/** Its address space: eighteen bits of nibble address is 256 KB of bytes. */
	constexpr std::size_t kMaxRomBytes = 256u << 10;

	/** Six bytes per entry: three of start, three of stop, big-endian, at `command * 8`. */
	constexpr uint32_t kTableStride = 8;

	/** The oscillator divides by 165, or by 132 when the board's pin 7 is strapped high - the
		same two constants the decoder uses. */
	constexpr double kDivisorPin7Low = 165.0;
	constexpr double kDivisorPin7High = 132.0;

	constexpr uint32_t kDefaultClock = 1000000u;
}

std::vector<playback::DecodedSample> collectOkim6295Samples (const midi::SongIr& ir,
															 uint32_t clockHz)
{
	SampleCollector collector;

	const std::vector<uint8_t> rom = memory::rom (ir, kRomType, kMaxRomBytes, false);

	if (rom.empty ())
	{
		return {};
	}

	// **The clock the device was started with, not the one the file declares.** A VGM may leave
	// its chip clock at zero and let the player pick; when it does, `SongIr` has nothing and any
	// fallback invented here is a different number from the one the row measured its rate with.
	// Measured on `02 Repeated Tragedy`: the row said 12396.7 Hz, this said 12121.2 - the default
	// 1 MHz against the player's 1022727, which is 2.3% flat, a third of a semitone.
	//
	// So the caller passes what it resolved for the device (`VgmReplayer` reads
	// `devCfg->clock`), and the only value used here is the one that arrives.
	const uint32_t clock = (clockHz != 0) ? clockHz : kDefaultClock;

	/** Per device instance: the command port is a two-byte handshake, so a voice start only means
		something once a sample number is pending, and the pin 7 strap changes the rate for the
		whole chip. */
	struct Instance
	{
		int pendingCommand = -1;
		bool pin7High = false;
	};

	std::map<uint32_t, Instance> instances;

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::okim6295)
		{
			continue;
		}

		if (write.port != 0)
		{
			continue;	// the second chip of a pair is a separate instance
		}

		Instance& instance = instances[write.deviceId];
		const uint8_t value = (uint8_t) write.value;

		if (write.reg == 0x0C)
		{
			instance.pin7High = (value & 0x01) != 0;
			continue;
		}

		if (write.reg != 0x00)
		{
			continue;	// the clock and bank registers do not name a sample
		}

		if (instance.pendingCommand < 0)
		{
			// Bit 7 marks the byte that means "this is a sample number"; without one pending, the
			// same bit pattern is the stop mask.
			if ((value & 0x80) != 0)
			{
				instance.pendingCommand = value & 0x7F;
			}

			continue;
		}

		const int command = instance.pendingCommand;
		instance.pendingCommand = -1;

		// Bits 4-7 are the voice mask. Any of them set is a key-on; the sample is named by the
		// command, so two voices starting one drum are one file and `add` drops the repeat.
		if ((value & 0xF0) == 0)
		{
			continue;
		}

		const uint32_t base = (uint32_t) command * kTableStride;

		if (base + 6 > rom.size ())
		{
			continue;
		}

		const uint32_t start = ((uint32_t) rom[base] << 16 | (uint32_t) rom[base + 1] << 8
								| (uint32_t) rom[base + 2]) & 0x3FFFFu;
		const uint32_t stop = ((uint32_t) rom[base + 3] << 16 | (uint32_t) rom[base + 4] << 8
							   | (uint32_t) rom[base + 5]) & 0x3FFFFu;

		if (start >= stop)
		{
			continue;
		}

		// **These are byte addresses, and a sample is `stop - start + 1` bytes of them.**
		//
		// Read the other way - nibbles, so half as long - every exported sample comes out as the
		// first half of what the row plays, and on `02 Repeated Tragedy` a kick that the ROM stores
		// as two adjacent entries became four fragments. What settled it was the row: it decodes
		// `2 * (stop - start + 1)` nibbles from `start`, and that is the sound the user hears and
		// drags out. An export has to hand back the same recording the row does, so it uses the
		// same arithmetic rather than its own reading of the table.
		const uint32_t firstByte = start;
		const uint32_t lastByte = stop;

		if (lastByte < firstByte || lastByte >= rom.size ())
		{
			continue;
		}

		const std::size_t length = (std::size_t) (lastByte - firstByte + 1);

		// Two levels in every byte, so the rate a file has to declare is twice the byte rate.
		const double byteRate = (double) clock / (instance.pin7High ? kDivisorPin7High
																	: kDivisorPin7Low);
		const double levelRate = byteRate * 2.0;

		const uint32_t levels = (uint32_t) (length * 2);

		char name[40] = {};
		std::snprintf (name, sizeof (name), "okim_%03X_%u", (unsigned) command, (unsigned) levels);

		// Keyed by command, not by voice or by moment: the ROM table is immutable, so a sample
		// number names one recording for the whole song.
		collector.add (((uint64_t) write.deviceId << 32) | (uint64_t) command, name,
					   rom.data () + firstByte, length, SampleFormat::msm6295, levelRate);
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
