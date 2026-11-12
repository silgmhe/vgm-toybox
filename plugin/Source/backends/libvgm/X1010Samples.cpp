#include "X1010Samples.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>

#include "ChipMemoryAssembly.h"
#include "SampleCollector.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	constexpr int kVoices = 16;

	/** The PCM sample ROM, as VGM names it. */
	constexpr uint8_t kRomType = 0x91;
	constexpr std::size_t kRomBytes = 1u << 20;

	/** Seta boards run the part at 16 MHz; a header that names no clock means this one. */
	constexpr double kDefaultClock = 16000000.0;

	/** `x1_010.c:231` substitutes 4 for a zero frequency register; without it the rate is zero. */
	constexpr int kMinimumPcmFrequency = 4;

	struct Voice
	{
		uint8_t status = 0;
		uint8_t frequency = 0;
		uint8_t startReg = 0;
		uint8_t endReg = 0;
	};
}

std::vector<playback::DecodedSample> collectX1010Samples (const midi::SongIr& ir)
{
	SampleCollector collector;

	// The image is placed by offset, not concatenated: every `0x91` block declares the 1 MB ROM and
	// carries a slice of it, which is why `clipToContent` is false here.
	const std::vector<uint8_t> rom = memory::rom (ir, kRomType, kRomBytes, false);

	if (rom.empty ())
	{
		return {};
	}

	double clock = kDefaultClock;

	for (const auto& slot : ir.chips)
	{
		if (slot.kind == midi::ChipKind::x1_010 && slot.clockHz > 100000)
		{
			clock = (double) slot.clockHz;
		}
	}

	Voice voices[kVoices];

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::x1_010)
		{
			continue;
		}

		// The VGM command carries a 16-bit offset split over `port` and `reg`; only the channel
		// block below 0x80 names a voice.
		const int reg = ((int) write.port << 8) | (int) write.reg;

		if (reg < 0 || reg >= 0x80)
		{
			continue;
		}

		const int index = reg >> 3;
		const int field = reg & 0x07;

		if (index < 0 || index >= kVoices)
		{
			continue;
		}

		Voice& voice = voices[index];

		switch (field)
		{
		case 0:
		{
			const bool wasOn = (voice.status & 0x01) != 0;
			voice.status = (uint8_t) write.value;

			if ((voice.status & 0x01) == 0 || wasOn)
			{
				continue;			// key-off, or still the same hit
			}

			// Only the PCM half has bytes to export; a waveform channel walks an on-chip table.
			if ((voice.status & 0x02) != 0)
			{
				continue;
			}

			const uint32_t start = (uint32_t) voice.startReg << 12;
			const uint32_t end = (uint32_t) (0x100 - voice.endReg) << 12;

			if (end <= start || start >= rom.size ())
			{
				continue;
			}

			const std::size_t length = (std::min<std::size_t>) (end - start, rom.size () - start);

			if (length < 2)
			{
				continue;
			}

			const int div = (voice.status & 0x80) != 0 ? 1 : 0;
			const int freq = std::max ((int) (voice.frequency >> div), kMinimumPcmFrequency);
			const double rate = clock / 8192.0 * (double) freq;

			// **One file per sample, not per voice**: two channels on the same drum are one entry,
			// and the first rate seen wins - the rule every other collector here follows.
			char name[32] = {};
			std::snprintf (name, sizeof (name), "x1010_%06X_%05X", (unsigned) start,
						   (unsigned) length);

			collector.add (((uint64_t) start << 32) | (uint64_t) length, name,
						   rom.data () + start, length, SampleFormat::pcm8Signed, rate);
			break;
		}

		case 2: voice.frequency = (uint8_t) write.value; break;
		case 4: voice.startReg = (uint8_t) write.value; break;
		case 5: voice.endReg = (uint8_t) write.value; break;
		default: break;
		}
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
