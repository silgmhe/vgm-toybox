#include "SegaPcmSamples.h"

#include <algorithm>
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
	/** The block type VGM gives a Sega PCM image, and the one `SegaPcmDecoder::kRequirements` names -
		so this is the same picture the decoder was handed. */
	constexpr uint8_t kRomType = 0x80;

	/** The chip addresses 24 bits, and the bank bits of the control register extend that; the image
		a song declares is what `memory::rom` clamps to anyway. */
	constexpr std::size_t kMaxRomBytes = 8u << 20;

	constexpr int kVoices = 16;

	/** The low window holds eight registers per voice, the high window its position and control. */
	constexpr uint16_t kHighWindowBit = 0x80;
	constexpr uint16_t kOffsetMask = 0x7F;
	constexpr int kChannelShift = 3;
	constexpr uint16_t kRegisterMask = 0x07;

	constexpr double kDefaultClock = 8000000.0;

	/** The VGM header's SegaPCM interface bytes (0x3C / 0x3E) for every file in this collection: the
		bank lives in the control byte's high bits, shifted by 13 and masked with 0xF8. */
	constexpr uint8_t kBankShift = 13;
	constexpr uint8_t kBankMask = 0xF8;

	/** `clock / 128` output samples a second, one step of the position being 1/256 of a byte, so one
		unit of the pitch register moves `clock / 32768` bytes a second (`segapcm.c:229`). */
	constexpr double kBytesPerPitchPerClock = 1.0 / 32768.0;

	struct Voice
	{
		uint8_t volumeLeft = 0;
		uint8_t volumeRight = 0;
		uint8_t endReg = 0;
		uint8_t pitch = 0;

		uint8_t addressMid = 0;
		uint8_t addressHigh = 0;
		uint8_t control = 0xFF;		// the core resets this RAM to 0xFF (`segapcm.c:255`)

		bool keyed = false;
	};

	struct Pending
	{
		uint32_t base = 0;
		std::size_t length = 0;
		double rate = 0.0;
	};
}

std::vector<playback::DecodedSample> collectSegaPcmSamples (const midi::SongIr& ir, uint32_t clockHz)
{
	SampleCollector collector;

	const std::vector<uint8_t> rom = memory::rom (ir, kRomType, kMaxRomBytes, false);

	if (rom.empty ())
	{
		return {};
	}

	const double clock = (clockHz > 0) ? (double) clockHz : kDefaultClock;

	std::map<uint32_t, std::array<Voice, kVoices>> chips;
	std::map<uint64_t, Pending> pending;

	/** **The control byte carries the ROM bank in its high bits**, and leaving it out put every
	 *	voice at the wrong bytes: `03 Scene Select` writes `control = 0x0A` and `0x12` - bank 8 and
	 *	16, so offsets 0x10000 and 0x20000 - while the address registers only name 0x0000.
	 *
	 *	**The shift and mask are per file.** The VGM header's SegaPCM interface bytes (0x3C / 0x3E)
	 *	say how far the bank bits move and which of them are bank: these two songs declare 13 / 0xF8
	 *	and 12 / 0x00 (a zero mask meaning the format's default, 0x70, which libvgm substitutes).
	 *	The loader carries the pair on the chip slot, so read it from there and fall back to the
	 *	usual 13 / 0xF8 only when the song said nothing.
	 */
	uint8_t bankShift = 13;
	uint8_t bankMask = 0xF8;

	for (const auto& slot : ir.chips)
	{
		if (slot.kind == midi::ChipKind::segapcm && slot.flags != 0)
		{
			bankShift = (uint8_t) (slot.flags & 0xFF);
			bankMask = (uint8_t) ((slot.flags >> 8) & 0xFF);
		}
	}

	const auto offsetOf = [bankShift, bankMask] (const Voice& voice)
	{
		return (uint32_t) (voice.control & bankMask) << bankShift;
	};

	/** **Filed by the bytes, not by the channel.** Two voices playing one drum, and the same sample
	 *	latching again later, are one file; the range plus its rate identifies it, and the rate is in
	 *	the key because a range retuned is a different playback (the collector de-duplicates the bytes
	 *	again by content).
	 */
	const auto keyOf = [offsetOf] (const Voice& voice)
	{
		const uint32_t start = offsetOf (voice) | (((uint32_t) voice.addressHigh << 8) | voice.addressMid);
		const uint32_t end = (uint32_t) (voice.endReg + 1) << 8;
		return ((uint64_t) start << 32) | (uint64_t) (end >> 8);
	};

	/** The key-on edge, recorded once per press: `segapcm.c:154` plays while control bit 0 is clear,
		so the write that clears it *after* it was set is the start - and the chip's RAM begins at 0xFF,
		which makes a driver's first clear a start too. */
	const auto latch = [&rom, &pending, &keyOf, offsetOf, clock] (const Voice& voice, double rate)
	{
		const uint32_t start = offsetOf (voice)
							 | (((uint32_t) voice.addressHigh << 8) | voice.addressMid);
		const uint32_t end = offsetOf (voice) | ((uint32_t) (voice.endReg + 1) << 8);

		if (end <= start || start >= rom.size ())
		{
			return;
		}

		const std::size_t length = (std::size_t) std::min<uint32_t> (end - start,
																	(uint32_t) (rom.size () - start));

		if (length < 2)
		{
			return;
		}

		Pending& entry = pending[keyOf (voice)];

		if (entry.length == 0)
		{
			entry.base = start;
			entry.length = length;
		}

		entry.rate = rate;
	};

	/** Bytes a second at the current pitch: zero when the register is, which the chip treats as a
		stopped position and the export as "no file". */
	const auto rateOf = [clock] (const Voice& voice)
	{
		return (double) voice.pitch * clock * kBytesPerPitchPerClock;
	};

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::segapcm)
		{
			continue;
		}

		auto& voices = chips[write.deviceId];

		// The command carries the address, and the chip splits it into two windows the way the
		// decoder does: bit 7 selects the high one.
		const uint16_t offset = (uint16_t) (((uint16_t) write.port << 8) | write.reg);
		const bool highWindow = (offset & kHighWindowBit) != 0;
		const int channel = (int) ((offset & kOffsetMask) >> kChannelShift);

		if (channel < 0 || channel >= kVoices)
		{
			continue;
		}

		Voice& voice = voices[(std::size_t) channel];
		const uint16_t which = offset & kRegisterMask;

		if (! highWindow)
		{
			switch (which)
			{
			case 0x02: voice.volumeLeft = write.value; break;
			case 0x03: voice.volumeRight = write.value; break;
			case 0x06: voice.endReg = write.value; break;

			case 0x07:
				// **A rate can arrive after the key-on and can change during one.** The drivers here
				// retune a voice mid-sample, so the *last* value wins - the one the row reports and
				// therefore the one the file has to declare.
				voice.pitch = write.value;

				if (voice.keyed && (voice.volumeLeft & 0x7F) + (voice.volumeRight & 0x7F) > 0)
				{
					const auto found = pending.find (keyOf (voice));

					if (found != pending.end ())
					{
						found->second.rate = rateOf (voice);
					}
				}

				break;

			default:
				break;		// 0x00/0x01 are unused, 0x04/0x05 the loop address
			}

			continue;
		}

		switch (which)
		{
		case 0x04: voice.addressMid = write.value; break;
		case 0x05: voice.addressHigh = write.value; break;

		case 0x06:
		{
			// **Every write that leaves bit 0 clear is a hit, not just the falling edge.**

			// The core has no edge at all: it rebuilds the address from 0x84/0x85 on every sample,
			// so a driver that writes the start registers and then repeats the same control byte
			// starts the voice again. `01 Magical Sound Shower` does exactly that - 7 583 writes
			// with the bit clear and only 6 falling edges - and the edge-only rule filed 3 ranges
			// where the stream names 12 (`docs/research/2026-11-12-segapcm-registers-forensics.md`).

			// The latch needs the whole state, which is why it happens on this write and not on the
			// address bytes: 0x84 and 0x85 arrive one at a time, and a hit taken between them would
			// pair one sample's high byte with another's low one.
			voice.control = write.value;
			voice.keyed = (write.value & 0x01) == 0;

			if (voice.keyed && (voice.volumeLeft & 0x7F) + (voice.volumeRight & 0x7F) > 0)
			{
				latch (voice, rateOf (voice));
			}

			break;
		}

		default:
			break;
		}
	}

	// **Filed at the end.** A sample's rate may be written after the address that named it, and
	// `add` keeps the first entry for a key, so the decision waits for every write.
	for (const auto& [key, entry] : pending)
	{
		if (entry.length < 2 || entry.rate <= 0.0)
		{
			continue;
		}

		const uint32_t start = (uint32_t) (key >> 32);

		// **Start and length, because one start can be several samples.** A driver that splits a
		// region into two hits writes two different end registers against the same address, and a
		// name built from the start alone filed both as `segapcm_00000`.
		char name[40] = {};
		std::snprintf (name, sizeof (name), "segapcm_%05X_%05X", (unsigned) start,
					   (unsigned) entry.length);

		collector.add (key, name, rom.data () + entry.base, entry.length,
					   SampleFormat::pcm8Unsigned, entry.rate);
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
