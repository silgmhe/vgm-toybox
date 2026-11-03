#include "Rf5c68Samples.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <map>
#include <set>
#include <utility>

#include "ChipMemoryAssembly.h"
#include "SampleCollector.h"
#include "midi/SongIr.h"
#include "chips/Rf5c68Decoder.h"

namespace toybox::backends::libvgm
{
namespace
{
	constexpr int kChannels = 8;

	/** The RAM the chip addresses, sixteen bits wide - the same ceiling the decoder uses. */
	constexpr std::size_t kRamBytes = 64 * 1024;

	/** A PCM bank in VGM: blocks below 0x40 concatenate, no header. */
	constexpr uint8_t kPcmBankType = 0x02;

	/** A RAM write in VGM: 0xC0-0xDF carry a 16-bit address and the bytes that belong there. */
	constexpr uint8_t kRamBlockType = 0xC1;

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
		uint64_t hash = 0;		// of the bytes as they were at this key-on: the same address can
								// hold different samples at different times, and only the content
								// tells them apart
		std::vector<uint8_t> bytes;	// copied only the first time a hash appears - the export must
									// have the bytes *as they were*, and the RAM keeps moving
	};
}

std::vector<playback::DecodedSample> collectRf5cSamples (const midi::SongIr& ir)
{
	SampleCollector collector;

	// A PCM bank, not a ROM block: `0x67` blocks of type 0x02, concatenated in stream order. The
	// runtime hands the decoder the same image through `setRam`. **This is only the state at load
	// time** - see the merge below for what changes during the song.
	std::vector<uint8_t> ram = memory::pcmBank (ir, kPcmBankType, kRamBytes);

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

	std::vector<Hit> hits;

	/** Which contents have already been copied, so a repeat key-on costs a hash and nothing else. */
	std::set<uint64_t> seenContents;

	/** Records that a channel started at an address; the length is settled afterwards.

		Scanning to a `0xFF` is the wrong way to size a sample here, and the first version of this
		file did it. On a Sonic CD track the samples contain no `0xFF` at all - what they reach is
		the padding at address 0, which the hardware arrives at by wrapping past 0xFFFF. So every
		sample scanned to the top of RAM and the export became six files that all ended at 0x10000,
		each one containing the samples that came after it.

		What the layout actually says: the six starts are `0x2000`, `0x4000`, `0x6000`, `0x8000`,
		`0xA000`, `0xE000` - every one of them a multiple of 0x2000, eight kilobytes apart. That is
		the bank's slot size, so a sample runs from its start to the next one. Deciding it after the
		whole stream has been read is the only way to know the neighbour.
	*/
	const auto collect = [&] (const Voice& voice)
	{
		// `0x06` counts 256-byte units, so the address is the register shifted up.
		const uint32_t address = (uint32_t) voice.start << 8;

		if (address >= ram.size () || voice.step == 0)
		{
			return;
		}

		// A start that lands on the 0xFF padding is a page nobody uploaded yet - or a channel the
		// driver never gave a sample to. Either way there is nothing here to hand to a listener.
		if (ram[address] == 0xFF)
		{
			return;
		}

		// To the first `0xFF` inside the slot, or the whole slot when there is none. This covers
		// both shapes the bank has: a streamed page ends where the next un-uploaded page begins
		// (it is still `0xFF` padding), and a slot-length sample contains no `0xFF` at all, so it
		// keeps the eight kilobytes `kSlotBytes` gives it.
		const uint32_t limit = (std::min) (address + chips::Rf5c68Decoder::kSlotBytes,
										   (uint32_t) ram.size ());
		std::size_t length = limit - address;

		for (std::size_t i = 0; address + i < limit; ++i)
		{
			if (ram[address + i] == 0xFF)
			{
				length = i;
				break;
			}
		}

		if (length < 2)
		{
			return;
		}

		// The bytes as they are *now*. The same address holds different samples at different
		// moments on a streaming driver, so the key-on's content is the only identity a sample has.
		uint64_t hash = 1469598103934665603ull ^ (uint64_t) address;

		for (std::size_t i = 0; i < length; ++i)
		{
			hash ^= ram[address + i];
			hash *= 1099511628211ull;
		}

		hits.push_back (Hit { address, length, voice.step, hash, {} });

		// The bytes travel with the first hit of each content: a slot triggered three hundred
		// times would otherwise copy eight kilobytes three hundred times over, all of them the
		// same eight kilobytes.
		if (seenContents.insert (hash).second)
		{
			hits.back ().bytes.assign (ram.begin () + address, ram.begin () + address + length);
		}
	};

	// The chip's RAM is not the load-time image for the whole song. A `0x67` block of type 0xC1 is
	// a **RAM write** (libvgm's `case 0xC0`, `Cmd_DataBlock`): a 16-bit address and the bytes that
	// go there, applied at the block's position in the command stream. On `01 - Palmtree Panic
	// Zone Past` there are 106 of them, all inside the first four kilobytes - the driver streams
	// 512-byte pages into that region and plays them, which is why the load-time image showed
	// padding where the piano actually was.
	//
	// Skipping them was the bug behind every report on this chip: the export handed back the
	// initial contents while the chip played the rewritten ones.
	std::vector<const midi::RomBlock*> ramWrites;

	for (const auto& block : ir.roms)
	{
		if (block.memoryType == kRamBlockType)
		{
			ramWrites.push_back (&block);
		}
	}

	const auto applyRam = [&ram] (const midi::RomBlock& block)
	{
		if (block.ramOffset >= ram.size () || block.size == 0)
		{
			return;
		}

		const std::size_t count = (std::min) (block.size, ram.size () - block.ramOffset);
		std::memcpy (ram.data () + block.ramOffset, block.data, count);
	};

	// One pass over the writes and the RAM blocks together, ordered by `streamOrder` - the file's
	// own sequence, not the clock. A block and the key-on that plays it share a `sampleTime`
	// (nothing waits between an upload and the note that uses it), so time cannot put them in
	// order; the position in the file can, and both carry it.
	std::size_t writeIndex = 0;
	std::size_t blockIndex = 0;

	while (writeIndex < ir.writes.size () || blockIndex < ramWrites.size ())
	{
		const bool doWrite = (blockIndex >= ramWrites.size ())
						  || (writeIndex < ir.writes.size ()
							  && ir.writes[writeIndex].streamOrder < ramWrites[blockIndex]->streamOrder);

		if (! doWrite)
		{
			applyRam (*ramWrites[blockIndex++]);
			continue;
		}

		const auto& write = ir.writes[writeIndex++];

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
					// The step is latched here because the driver sets it **before** enabling:
					// measured on `01 - Palmtree Panic Zone Past`, every note reports the same value
					// on and off (657/657, 1108/1108), so reading it at the key-on is not a race.
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

	// One file per distinct **content**, not per address: a streamed page is a different sample
	// every time the driver rewrites it, and the bytes a key-on found are the only identity it has.
	//
	// The rate is the **most frequent** step within that content, not the highest: a slot is
	// triggered hundreds of times at many pitches (299 hits on one, steps 619..2503), so any single
	// choice is a transposition, and the one the driver uses most is the least surprising.
	std::map<uint64_t, std::map<uint16_t, int>> histograms;
	std::map<uint64_t, const Hit*> byContent;

	for (const auto& hit : hits)
	{
		++histograms[hit.hash][hit.step];

		if (! hit.bytes.empty ())
		{
			byContent.emplace (hit.hash, &hit);
		}
	}

	// The same address can hold several contents over a song; the second one onwards gets a
	// version suffix so the files do not overwrite each other.
	//
	// Emission walks the addresses in order rather than the hashes: a caller listing the files sees
	// them sorted the way the bank is, and a test that says "the first file is the one at 0x0200"
	// stays true.
	std::vector<const Hit*> ordered;

	for (const auto& entry : byContent)
	{
		ordered.push_back (entry.second);
	}

	std::sort (ordered.begin (), ordered.end (),
			   [] (const Hit* a, const Hit* b) { return a->address < b->address; });

	std::map<std::pair<uint32_t, std::size_t>, int> versions;

	for (const Hit* hitPointer : ordered)
	{
		const Hit& hit = *hitPointer;

		const auto& histogram = histograms[hit.hash];
		int best = 0;
		uint16_t step = hit.step;

		for (const auto& candidate : histogram)
		{
			if (candidate.second > best)
			{
				best = candidate.second;
				step = candidate.first;
			}
		}

		// `step * (clock/384) / 2048` bytes a second - the same arithmetic the decoder's
		// `bytesPerSecond` does.
		const double rate = (double) step * clock / kOutputDivisor / kStepPerByte;

		const int version = ++versions[std::make_pair (hit.address, hit.length)];

		char name[40] = {};

		if (version == 1)
		{
			std::snprintf (name, sizeof (name), "rf5c_%06X_%u", (unsigned) hit.address,
						   (unsigned) hit.length);
		}
		else
		{
			std::snprintf (name, sizeof (name), "rf5c_%06X_%u_v%d", (unsigned) hit.address,
						   (unsigned) hit.length, version);
		}

		collector.add (hit.hash, name, hit.bytes.data (), hit.length,
					   SampleFormat::signMagnitude8, rate);
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
