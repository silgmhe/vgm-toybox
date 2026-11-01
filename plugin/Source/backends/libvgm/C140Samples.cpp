#include "C140Samples.h"

#include <algorithm>
#include <cstdio>
#include <map>

#include "ChipMemoryAssembly.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	constexpr int kVoices = 24;

	/** Twelve registers in a row: two volumes, two frequency bytes, then bank, mode, start, end and
		loop, two bytes each. The offsets here are the low nibble of the register number. */
	constexpr int kRegistersPerVoice = 0x0C;

	/** The C140's sample ROM, as VGM calls it. */
	constexpr uint8_t kRomType = 0x8D;

	/** The chip's reference rate, the one its pitch is measured against. A per-voice rate exists
		but belongs to the moment, not to the sample. */
	constexpr double kReferenceRate = 8000.0;

	/** libvgm's `find_sample()` (c140.c), copied deliberately rather than shared: the decoder keeps
		this arithmetic private, and a second reader that drifted from it would address different
		bytes than the voice actually plays. The tests compare the two paths on one ROM. */
	uint32_t romOffset (uint32_t address, uint8_t bank, uint8_t banking)
	{
		const uint32_t adrs = ((uint32_t) bank << 16) + address;

		switch (banking)
		{
		case 0x01:		// system21
			return ((adrs & 0x300000u) >> 1) + (adrs & 0x7FFFFu);

		case 0x03:		// linear
			return adrs;

		case 0x00:		// system2, and the default
		default:
			return ((adrs & 0x200000u) >> 2) | (adrs & 0x7FFFFu);
		}
	}

	struct Voice
	{
		uint8_t bank = 0;
		uint8_t mode = 0;
		uint32_t start = 0;
		uint32_t end = 0;
	};
}

std::vector<playback::DecodedSample> collectC140Samples (const midi::SongIr& ir)
{
	std::vector<playback::DecodedSample> out;

	// The assembler the MIDI path uses, rather than a second one here: it was extracted for exactly
	// this job, and a copy would drift from it without anything failing.
	//
	// `clipToContent` is true because the C140's blocks declare no size of their own - the image is
	// as long as the highest block reaches - and 16 MB is its whole address space, which is also the
	// ceiling this needs.
	const std::vector<uint8_t> rom = memory::rom (ir, kRomType, 16u << 20, true);

	if (rom.empty ())
	{
		return out;
	}

	uint8_t banking = 0x00;		// system2, which is also what an unread header means

	for (const auto& slot : ir.chips)
	{
		if (slot.kind == midi::ChipKind::c140)
		{
			banking = (uint8_t) slot.flags;
		}
	}

	Voice voices[kVoices];
	std::map<uint32_t, bool> seen;

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::c140)
		{
			continue;
		}

		const int index = (int) write.reg / kRegistersPerVoice;
		const int reg = (int) write.reg % kRegistersPerVoice;

		if (index < 0 || index >= kVoices)
		{
			continue;
		}

		Voice& voice = voices[index];

		switch (reg)
		{
		case 0x04: voice.bank = (uint8_t) write.value; break;
		case 0x05: voice.mode = (uint8_t) write.value; break;
		case 0x06: voice.start = (voice.start & 0x00FFu) | ((uint32_t) write.value << 8); break;
		case 0x07: voice.start = (voice.start & 0xFF00u) | (uint32_t) write.value; break;
		case 0x08: voice.end = (voice.end & 0x00FFu) | ((uint32_t) write.value << 8); break;
		case 0x09: voice.end = (voice.end & 0xFF00u) | (uint32_t) write.value; break;
		default: break;
		}

		// The key-on bit is written with the mode, so this is where a voice starts sounding - the
		// same edge `ChipState::hitCount` counts in the decoder.
		if (reg != 0x05 || (voice.mode & 0x80) == 0)
		{
			continue;
		}

		// mu-law is not a level. Drawing it as one would be a shape this chip cannot make, which is
		// the same reason the decoder leaves those voices' waves null.
		if ((voice.mode & 0x08) != 0 || voice.end <= voice.start)
		{
			continue;
		}

		const uint32_t offset = romOffset (voice.start, voice.bank, banking);

		if (offset >= rom.size ())
		{
			continue;
		}

		const std::size_t length = (std::size_t) (voice.end - voice.start);
		const std::size_t count = (std::min) (length, rom.size () - offset);

		if (count < 2)
		{
			continue;
		}

		// One entry per sample, not per voice: two voices on the same drum are one file.
		const uint32_t key = (offset << 8) ^ (uint32_t) count;

		if (! seen.emplace (key, true).second)
		{
			continue;
		}

		char name[32] = {};
		std::snprintf (name, sizeof (name), "c140_%05X_%u", (unsigned) offset, (unsigned) count);

		playback::DecodedSample sample;
		sample.name = name;
		sample.values.reserve (count);

		// Eight-bit signed samples, which is what this chip stores.
		for (std::size_t i = 0; i < count; ++i)
		{
			sample.values.push_back ((float) (int8_t) rom[offset + i] / 128.0f);
		}

		sample.rate = kReferenceRate;

		out.push_back (std::move (sample));
	}

	return out;
}

} // namespace toybox::backends::libvgm
