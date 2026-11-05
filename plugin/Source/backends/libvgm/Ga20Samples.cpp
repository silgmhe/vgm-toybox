#include "Ga20Samples.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <map>

#include "ChipMemoryAssembly.h"
#include "SampleCollector.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	/** The GA20's ROM, as VGM calls it - the type its `kRequirements` names, so the image here is
		the one the decoder was handed. */
	constexpr uint8_t kRomType = 0x93;

	/** Twenty address bits: the start register is `data << 4` and `data << 12`. */
	constexpr std::size_t kMaxRomBytes = 1u << 20;

	/** A sample that has not ended within this much of the image has not been understood; the
		same ceiling the decoder scans with. */
	constexpr uint32_t kMaxSampleScan = 256u * 1024;

	/** `iremga20.c:367` - the device runs at a quarter of the chip clock. */
	constexpr double kClockDivisor = 4.0;

	constexpr uint32_t kDefaultClock = 3579545u;

	/** A voice's registers: six of the eight are written by a driver, and only these four mean
		anything to a sample. */
	struct Voice
	{
		uint16_t start = 0;		// in 16-byte units, exactly as the two registers hold it
		uint8_t rate = 0;
		bool keyed = false;
		uint32_t latchStart = 0;	// byte address at the key-on
	};

	/** What a sample is, once a control write has started it. */
	struct Pending
	{
		std::vector<uint8_t> bytes;
		std::size_t length = 0;
		double rate = 0.0;
	};

	/** The power-of-two mask the decoder folds an address with. The ROM is covered as a
		power-of-two buffer, so this mirrors it rather than reading past the vector. */
	uint32_t pow2Mask (std::size_t size)
	{
		if (size == 0)
		{
			return 0;
		}

		uint32_t mask = (uint32_t) (size - 1);
		mask |= mask >> 1;
		mask |= mask >> 2;
		mask |= mask >> 4;
		mask |= mask >> 8;
		mask |= mask >> 16;
		return mask;
	}
}

std::vector<playback::DecodedSample> collectGa20Samples (const midi::SongIr& ir, double rateHz,
														 uint32_t clockHz)
{
	SampleCollector collector;

	const std::vector<uint8_t> rom = memory::rom (ir, kRomType, kMaxRomBytes, false);

	if (rom.empty ())
	{
		return {};
	}

	const uint32_t romMask = pow2Mask (rom.size ());

	const auto byteAt = [&rom, romMask] (uint32_t address)
	{
		const uint32_t index = address & romMask;
		return (index < rom.size ()) ? rom[index] : (uint8_t) 0;
	};

	/** The device's rate, or the arithmetic when it never started. Both are `clock / 4`; the first
		is preferred because it is what the render used, which is the rule the OKIM6295 taught. */
	uint32_t clock = clockHz;

	if (clock == 0)
	{
		for (const auto& slot : ir.chips)
		{
			if (slot.kind == midi::ChipKind::ga20 && slot.clockHz != 0)
			{
				clock = slot.clockHz;
			}
		}
	}

	const double byteClock =
		(rateHz > 0.0) ? rateHz : (double) ((clock != 0) ? clock : kDefaultClock) / kClockDivisor;

	/** One byte every `256 - rate` samples of the device's own clock. */
	const auto rateFor = [byteClock] (uint8_t rate)
	{
		return byteClock / (double) (0x100 - rate);
	};

	std::map<uint32_t, std::array<Voice, 4>> chips;
	std::map<uint32_t, Pending> pending;

	/** Files a keyed voice's bytes. The address is the identity: one GA20 per song, one image. */
	const auto emit = [&] (Voice& voice, double rate)
	{
		const uint32_t start = voice.latchStart;

		if (byteAt (start) == 0x00)
		{
			return;		// the core clears `play` for a sample that begins on its own terminator
		}

		std::size_t length = 0;

		while (length < kMaxSampleScan && byteAt (start + (uint32_t) length) != 0x00)
		{
			++length;
		}

		// **No terminator, no sample.** Every real sample ends at a 0x00 byte; running to the
		// ceiling means the address was misread or the region is padding, and the K007232 taught
		// what writing those bytes out looks like - a file no player will open.
		if (length < 2 || length >= kMaxSampleScan)
		{
			return;
		}

		Pending& entry = pending[start];

		if (entry.length == 0)
		{
			entry.bytes.assign (rom.begin () + (long) start,
								rom.begin () + (long) (start + length));
			entry.length = length;
		}

		// The rate is refreshed by every later key-on and by every later rate write: a driver may
		// write the pitch after the control byte, and may retune mid-sample. The last value is what
		// the row reports, so it is what the file has to declare.
		entry.rate = rate;
	};

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::ga20)
		{
			continue;
		}

		const uint8_t offset = (uint8_t) (write.reg & 0x1F);
		const int channel = offset >> 3;

		if (channel < 0 || channel >= 4)
		{
			continue;
		}

		auto& voices = chips[write.deviceId];
		Voice& voice = voices[(std::size_t) channel];

		switch (offset & 0x07)
		{
		case 0:		// start, low: bits 4..11 of the byte address
			voice.start = (uint16_t) ((voice.start & 0xFF00) | (write.value & 0xFF));
			break;

		case 1:		// start, high: bits 12..19
			voice.start = (uint16_t) ((voice.start & 0x00FF) | ((uint16_t) write.value << 8));
			break;

		case 2:		// the end registers: written by every driver, read by nobody
		case 3:
			break;

		case 4:		// rate
			voice.rate = (uint8_t) write.value;

			// A retune of the sample that is already playing: the row reads this register live, so
			// the file follows it. Keyed by the latched address, not the pending one, so a driver
			// that writes the next start before the next control cannot retune the wrong sample.
			if (voice.keyed)
			{
				const auto found = pending.find (voice.latchStart);

				if (found != pending.end ())
				{
					found->second.rate = rateFor (voice.rate);
				}
			}

			break;

		case 5:		// volume: the row reports it, the file has no use for it
			break;

		case 6:		// control: bit 1 starts the voice, anything else stops it
			if ((write.value & 0x02) == 0)
			{
				voice.keyed = false;
				break;
			}

			voice.latchStart = (uint32_t) voice.start << 4;
			voice.keyed = true;
			emit (voice, rateFor (voice.rate));
			break;

		default:	// 7 is read-only status
			break;
		}
	}

	for (auto& [start, entry] : pending)
	{
		if (entry.length < 2 || entry.rate <= 0.0)
		{
			continue;
		}

		char name[32] = {};
		std::snprintf (name, sizeof (name), "ga20_%05X", (unsigned) start);

		// The bytes travel to `SampleCollector` untouched, which decodes them for the file - the
		// K007232's rule, and the reason a format that is *almost* int8 gets its own enum entry
		// rather than a special case at the call site.
		collector.add (start, name, entry.bytes.data (), entry.length, SampleFormat::ga20,
					   entry.rate);
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
