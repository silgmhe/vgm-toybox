#include "MultiPcmSamples.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "ChipMemoryAssembly.h"
#include "SampleCollector.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	/** The VGM block type for this chip's samples. */
	constexpr uint8_t kRomType = 0x89;

	/** What the blocks declare, and the address space the metadata's 22-bit start can name. */
	constexpr std::size_t kMaxRomBytes = 4u << 20;

	/** `multipcm.c:129` - the chip's sample rate is the master clock over 224. */
	constexpr double kClockDivisor = 224.0;
	constexpr uint32_t kDefaultClock = 8000000u;

	/** One metadata entry: start, loop, negated length, then the envelope and LFO bytes. */
	constexpr uint32_t kMetaBytes = 12;

	struct Voice
	{
		uint16_t pitch = 0;
		int octave = 0;
		bool keyed = false;
		int sampleIndex = 0;

		/** What the key-on latched: the bytes, their length in samples, and their base in the image. */
		uint32_t base = 0;
		uint32_t length = 0;
	};

	struct Pending
	{
		std::vector<uint8_t> bytes;
		std::size_t length = 0;
		double rate = 0.0;
	};

	/** `multipcm.c:314-324`: octave 1 is unity, and the field is four-bit signed. */
	double octaveFactor (int octave)
	{
		const int shifted = (octave - 1) & 0x0F;

		if ((shifted & 0x8) != 0)
		{
			return std::ldexp (1.0, -(16 - shifted));
		}

		return std::ldexp (1.0, shifted);
	}

	double ratioOf (int octave, int pitch)
	{
		return ((1024.0 + (double) pitch) / 1024.0) * octaveFactor (octave);
	}

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

std::vector<playback::DecodedSample> collectMultiPcmSamples (const midi::SongIr& ir, double rateHz,
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

	const double chipRate = (rateHz > 0.0)
						  ? rateHz
						  : (double) ((clockHz != 0) ? clockHz : kDefaultClock) / kClockDivisor;

	/** `multipcm.c:671` plus the octave shift: samples (bytes, for 8-bit) per second. */
	const auto rateFor = [chipRate] (const Voice& voice)
	{
		return chipRate * ratioOf (voice.octave, (int) voice.pitch);
	};

	struct Chip
	{
		Voice voices[28];
		int currentSlot = -1;
		int address = 0;
		bool segaBanking = false;
		uint32_t bank0 = 0;
		uint32_t bank1 = 0;
	};

	std::map<uint32_t, Chip> chips;
	std::map<uint64_t, Pending> pending;

	/** `multipcm.c:281-298`: the metadata entry for one index. */
	const auto readMeta = [&byteAt] (int index, uint32_t& start, uint32_t& length, uint8_t& format)
	{
		const uint32_t at = (uint32_t) index * kMetaBytes;

		const uint32_t raw = ((uint32_t) byteAt (at) << 16) | ((uint32_t) byteAt (at + 1) << 8)
						   | (uint32_t) byteAt (at + 2);
		const uint32_t negated = ((uint32_t) byteAt (at + 5) << 8) | (uint32_t) byteAt (at + 6);

		format = (uint8_t) ((raw >> 20) & 0xFE);
		start = raw & 0x3FFFFF;
		length = 0x10000 - negated;
	};

	/** `multipcm.c:733-743` - the banked window, which every sample here needs. */
	const auto mappedBase = [] (const Chip& chip, uint32_t start)
	{
		if (! chip.segaBanking)
		{
			return start;
		}

		const uint32_t window = start & 0x1FFFFF;

		if ((window & 0x100000) == 0)
		{
			return window;
		}

		return (window & 0x07FFFF) | (((window & 0x080000) != 0) ? chip.bank1 : chip.bank0);
	};

	/** Files a keyed voice's sample. The image is shared by every instance of the type, so the
		address and length are the identity - the K007232's rule. */
	const auto emit = [&] (const Chip& chip, Voice& voice)
	{
		uint32_t start = 0;
		uint32_t length = 0;
		uint8_t format = 0;
		readMeta (voice.sampleIndex, start, length, format);

		// **Twelve-bit samples are left out.** `multipcm.c:849-864` packs two of them into three
		// bytes; no song in `test_file/YMW258` uses one, and half-understood packing would export
		// bytes that are not the sound. Nothing is the honest answer here.
		if ((format & 0x04) != 0)
		{
			return;
		}

		voice.base = mappedBase (chip, start);
		voice.length = (length > 0x10000u) ? 0x10000u : length;

		if (voice.base >= rom.size () || voice.length < 2)
		{
			return;
		}

		const std::size_t available = rom.size () - voice.base;
		const std::size_t count = std::min<std::size_t> (voice.length, available);

		if (count < 2)
		{
			return;
		}

		const uint64_t key = ((uint64_t) voice.base << 20) | (uint64_t) (voice.length & 0xFFFFF);
		Pending& entry = pending[key];

		if (entry.length == 0)
		{
			entry.bytes.assign (rom.begin () + (long) voice.base,
								rom.begin () + (long) (voice.base + count));
			entry.length = count;
		}

		// Refreshed by every later key-on and every later pitch write: the last value is what the
		// row reports and therefore what the file has to declare.
		entry.rate = rateFor (voice);
	};

	/** Retunes the sample a voice is already playing, for the writes that change its speed. */
	const auto retune = [&] (Voice& voice)
	{
		if (! voice.keyed || voice.length == 0)
		{
			return;
		}

		const auto found =
			pending.find (((uint64_t) voice.base << 20) | (uint64_t) (voice.length & 0xFFFFF));

		if (found != pending.end ())
		{
			found->second.rate = rateFor (voice);
		}
	};

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::multiPcm)
		{
			continue;
		}

		Chip& chip = chips[write.deviceId];

		// The loader turns VGM's 0xC3 into these three registers (`Cmd_YMW_Bank`).
		if (write.reg == 0x10)
		{
			chip.segaBanking = true;
			chip.bank0 = (uint32_t) write.value << 20;
			chip.bank1 = ((uint32_t) write.value << 20) | 0x080000u;
			continue;
		}

		if (write.reg == 0x11)
		{
			chip.segaBanking = true;
			chip.bank0 = (uint32_t) write.value << 19;
			continue;
		}

		if (write.reg == 0x12)
		{
			chip.segaBanking = true;
			chip.bank1 = (uint32_t) write.value << 19;
			continue;
		}

		if (write.reg == 0x01)
		{
			const int selector = write.value & 0x1F;

			if ((selector & 0x07) == 0x07)
			{
				chip.currentSlot = -1;
				continue;
			}

			chip.currentSlot = (selector >> 3) * 7 + (selector & 0x07);
			continue;
		}

		if (write.reg == 0x02)
		{
			chip.address = (write.value > 7) ? 7 : write.value;
			continue;
		}

		if (write.reg != 0x00 || chip.currentSlot < 0 || chip.currentSlot >= 28)
		{
			continue;
		}

		Voice& voice = chip.voices[(std::size_t) chip.currentSlot];

		switch (chip.address)
		{
		case 0x1:		// sample index: nine bits, shared with bit 0 of the pitch's low byte
			voice.sampleIndex =
				(int) ((uint32_t) write.value | ((uint32_t) (voice.pitch & 0x01) << 8));
			break;

		case 0x2:		// pitch, low
			voice.pitch = (uint16_t) ((voice.pitch & 0x300) | (write.value & 0xFF));
			voice.sampleIndex = (int) ((uint32_t) (voice.sampleIndex & 0xFF)
									   | ((uint32_t) (write.value & 0x01) << 8));
			retune (voice);
			break;

		case 0x3:		// four-bit signed octave above a ten-bit pitch
			voice.octave = (int8_t) (uint8_t) (write.value >> 4);
			voice.pitch = (uint16_t) ((voice.pitch & 0x0FF) | ((write.value & 0x03) << 8));
			retune (voice);
			break;

		case 0x4:		// bit 7 keys the voice on
		{
			const bool wasKeyed = voice.keyed;
			voice.keyed = (write.value & 0x80) != 0;

			if (voice.keyed && ! wasKeyed)
			{
				emit (chip, voice);
			}

			break;
		}

		default:		// 0x0 pan, 0x5 volume, 0x6/0x7 the LFOs
			break;
		}
	}

	for (auto& [key, entry] : pending)
	{
		if (entry.length < 2 || entry.rate <= 0.0)
		{
			continue;
		}

		char name[32] = {};
		std::snprintf (name, sizeof (name), "multipcm_%06X", (unsigned) (key >> 20));

		// The bytes are signed eight-bit, which is `SampleFormat`'s default branch: no new format,
		// because this chip needs no conversion.
		collector.add (key, name, entry.bytes.data (), entry.length, SampleFormat::pcm8Signed,
					   entry.rate);
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
