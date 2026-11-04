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

	/** `sampleTime`'s unit, which is what turns a gate into seconds. */
	constexpr double kStreamRate = 44100.0;

	/** How long a release may last and still be part of the same note.

		The driver rewrites the whole `0x08` mask between notes and drops a voice's bit on the way, so
		one note arrives as several enable/release pairs. Twenty milliseconds is longer than that
		rewrite and shorter than the gap between two notes of one channel in these rips.
	*/
	constexpr double kContinuation = 0.020;

	/** Notes are compared by length rounded up to this, so a few bytes of gate jitter do not make
	 *  a new file. A hundred and twenty-eight bytes is 12 ms at 10 kHz - finer than the difference
	 *  between two notes of one instrument, coarser than the jitter between two presses. */
	constexpr std::size_t kLengthBucket = 128;

	struct Voice
	{
		uint8_t start = 0;			// 0x06, in 256-byte units
		uint16_t step = 0;			// 0x02 / 0x03, the playback rate and the only pitch
		bool enabled = false;

		/** The state latched at the key-on, and when it happened.

			A note's length is not in the data. It is the time the channel stayed on, times the byte
			rate - and that is measurable against the emulator: `vgm_rf5c_probe` renders channel 5 of
			`01 - Palmtree Panic Zone Past` as a note **0.49 s long**, while the whole 8 KB slot at the
			rate the registers ask for is 0.78 s. The extra third of a second is the slot continuing
			past the end of the note, which is what the file used to contain.
		*/
		uint32_t enabledAt = 0;
		uint8_t armedStart = 0;
		uint16_t armedStep = 0;

		/** When this voice was last released, so a re-enable within a few milliseconds can be
			treated as the same note rather than a new one.

			The driver rewrites the whole `0x08` mask between notes, which drops this voice's bit to
			zero and back. Measured against the emulator, channel 5's note lasts 0.491 s while the
			gates between those toggles add up to 0.124 s - four fragments of one note, and the
			export was handing back the first fragment: a piano cut to a quarter of its length, which
			is exactly what "it sounds like a hi-hat" is.
		*/
		uint32_t releasedAt = 0;
		bool released = false;
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
		std::vector<uint8_t> bytes;	// copied only the first time a bucket appears - the export must
									// have the bytes *as they were*, and the RAM keeps moving
		std::size_t bucket = 0;		// `length` rounded up to `kLengthBucket`: two notes of one
									// instrument differ by a few bytes of gate time, so the exact
									// length is what turned one file into 1395
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

	// TEMP: what the first notes of 0x8000 look like - gate in bytes, and the step they used.
	std::vector<std::pair<std::size_t, uint16_t>> probeHits;

	/** Which contents have already been copied, so a repeat key-on costs a hash and nothing else. */
	std::set<std::pair<uint32_t, std::size_t>> seenContents;

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
	const auto collect = [&] (const Voice& voice, uint32_t now)
	{
		// `0x06` counts 256-byte units, so the address is the register shifted up.
		const uint32_t address = (uint32_t) voice.armedStart << 8;

		if (address >= ram.size () || voice.armedStep == 0)
		{
			return;
		}

		// A start that lands on the 0xFF padding is a page nobody uploaded yet - or a channel the
		// driver never gave a sample to. Either way there is nothing here to hand to a listener.
		if (ram[address] == 0xFF)
		{
			return;
		}

		const uint32_t limit = (std::min) (address + chips::Rf5c68Decoder::kSlotBytes,
										   (uint32_t) ram.size ());

		// **The gate.** How long the channel stayed on, converted to bytes at the rate this note
		// was played at. `sampleTime` counts 44.1 kHz samples.
		const double byteRate = (double) voice.armedStep * clock / kOutputDivisor / kStepPerByte;
		const double seconds = (double) (now - voice.enabledAt) / kStreamRate;
		std::size_t length = (std::size_t) (seconds * byteRate);

		length = (std::min) (length, (std::size_t) (limit - address));

		// A note still sounding when the stream ends has no measured length; the slot is the only
		// honest answer there, and it is also what a sampling driver loops.
		if (seconds <= 0.0)
		{
			length = limit - address;
		}

		if (length < 2)
		{
			return;
		}

		// The bytes as they are *now* - the same address holds different samples at different
		// moments, so the moment of the note is what distinguishes one from another.
		uint64_t hash = 1469598103934665603ull ^ (uint64_t) address;

		for (std::size_t i = 0; i < length; ++i)
		{
			hash ^= ram[address + i];
			hash *= 1099511628211ull;
		}

		// Lengths are bucketed before they are compared: two notes of one instrument differ by a
		// few bytes of gate time (a MIDI rip is not metronomic), so exact lengths make every note a
		// separate file - 1395 of them when this was first tried.
		const std::size_t bucket = ((length + kLengthBucket - 1) / kLengthBucket) * kLengthBucket;

		hits.push_back (Hit { address, length, voice.armedStep, hash, {}, bucket });

		// TEMP
		if (address == 0x8000u && probeHits.size () < 12)
		{
			probeHits.emplace_back (length, voice.armedStep);
		}

		if (seenContents.insert (std::make_pair (address, bucket)).second)
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

	// **The RAM bank, which the write address has to be or-ed with.**
	//
	// libvgm does this in `DoRAMOfsPatches`: `dataOfs |= _rf5cBank[1][chipID] << 12`, and that
	// register is set by `0x07` with bit 6 **clear** - `_rf5cBank[..] = value & 0x0F`. Our handler
	// only ever read the bit-6-**set** form, where the low three bits select the channel, so every
	// streamed page was written twelve bits too low. That is why a page the driver put in the 0x8000
	// window came out of the export as whatever was at 0x0000, and why no slot ever matched.
	uint8_t ramBank = 0;

	const auto applyRam = [&ram, &ramBank] (const midi::RomBlock& block)
	{
		const std::size_t address = (std::size_t) block.ramOffset | ((std::size_t) ramBank << 12);

		if (address >= ram.size () || block.size == 0)
		{
			return;
		}

		const std::size_t count = (std::min) (block.size, ram.size () - address);
		std::memcpy (ram.data () + address, block.data, count);
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
					//
					// **The release does not end a note by itself.** The driver rewrites the whole
					// mask between notes and drops this voice's bit on the way, so one note arrives
					// as several enable/release pairs; collecting at each release exported the first
					// fragment, which is a piano cut to a quarter of its length - "it sounds like a
					// hi-hat". A release that is picked up again within `kContinuation` continues the
					// note; one that is not ends it, and it is collected here, at its real end.
					if (voices[i].released
						&& (write.sampleTime - voices[i].releasedAt) > (uint32_t) (kContinuation * kStreamRate))
					{
						collect (voices[i], voices[i].releasedAt);
					}

					if (! voices[i].released
						|| (write.sampleTime - voices[i].releasedAt) > (uint32_t) (kContinuation * kStreamRate))
					{
						voices[i].armedStart = voices[i].start;
						voices[i].armedStep = voices[i].step;
						voices[i].enabledAt = write.sampleTime;
					}
				}
				else if (! on && voices[i].enabled)
				{
					voices[i].releasedAt = write.sampleTime;
					voices[i].released = true;
				}

				voices[i].enabled = on;
			}

			continue;
		}

		// **Two different registers share `0x07`, and bit 6 says which one.** With bit 6 set the low
		// three bits select the channel the low registers address; with bit 6 clear the low four
		// bits are the RAM bank that every memory write is or-ed with (libvgm's `DoRAMOfsPatches`).
		// Only the first form was handled here, so no write ever reached the window the driver put
		// it in.
		if (offset == 0x07)
		{
			if ((value & 0x40) != 0)
			{
				bank = (uint8_t) (value & 0x07);
			}
			else
			{
				ramBank = (uint8_t) (value & 0x0F);
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

	// **One file per address, cut to the length that address is usually held for.**
	//
	// The slot is a buffer, not a sample: the driver keeps writing into the same eight kilobytes,
	// so the whole slot is the note *plus* whatever follows it. Against a render of channel 5 on
	// `01 - Palmtree Panic Zone Past`, the note is 0.49 s and the slot is 0.78 s - the extra third
	// of a second is what made a bass sound like a loop of several instruments.
	//
	// Not one file per note either: every note of a channel lasts a few bytes longer or shorter
	// than the last, so exact lengths made 1395 files. The gate is bucketed (`kLengthBucket`) and
	// the most played bucket wins - one file per instrument, at the length it is heard for.
	struct Group
	{
		const Hit* first = nullptr;
		int count = 0;
		std::map<uint16_t, int> steps;
	};

	// The stream ends with notes still open - the last note of every channel at least. A released
	// voice has its end and is collected now; a voice still enabled has no measured end and is
	// collected by `collect`'s own fallback (the whole slot).
	for (int i = 0; i < kChannels; ++i)
	{
		if (voices[i].released)
		{
			collect (voices[i], voices[i].releasedAt);
		}
		else if (voices[i].enabled)
		{
			collect (voices[i], voices[i].enabledAt);
		}
	}

	std::map<uint32_t, std::map<std::size_t, Group>> byAddress;

	/** The bucket each address met first: its first note, which is the one that presents the sample. */
	std::map<uint32_t, std::size_t> firstBucket;

	for (const auto& hit : hits)
	{
		Group& group = byAddress[hit.address][hit.bucket];
		++group.count;
		++group.steps[hit.step];

		if (group.first == nullptr && ! hit.bytes.empty ())
		{
			group.first = &hit;
		}

		firstBucket.emplace (hit.address, hit.bucket);
	}

	// Sorted by address, so a caller listing the files sees the bank in order and a test that says
	// "the first file is the one at 0x0200" stays true.
	for (const auto& address : byAddress)
	{

		// **The first note this address is heard in**, not the most common one and not the longest.
		//
		// A sample is designed to be heard from its start, and the first time it appears is the one
		// that plays it as written. Measured on `01 - Palmtree Panic Zone Past`, `0x8000`'s first
		// note is 5186 bytes at 10443 Hz - 0.497 s, against 0.491 s for that channel rendered by
		// the emulator. The most common length there is 1294 bytes, a quarter of it, and a piano cut
		// to a quarter of its length is what was reported as "it sounds like a hi-hat"; the longest
		// is the whole 8 KB slot, which is the buffer rather than the note.
		const auto wanted = firstBucket.find (address.first);
		const auto bucket = (wanted != firstBucket.end ()) ? address.second.find (wanted->second)
														  : address.second.end ();

		const Group* chosen = (bucket != address.second.end ()) ? &bucket->second : nullptr;

		if (chosen == nullptr || chosen->first == nullptr)
		{
			continue;
		}

		// The most frequent step within that bucket - the pitch this instrument is usually played
		// at. A file has one rate and the driver transposes freely (one address runs 74..2224), so
		// any single choice is a transposition and the commonest is the least surprising.
		int best = 0;
		uint16_t step = chosen->first->step;

		for (const auto& candidate : chosen->steps)
		{
			if (candidate.second > best)
			{
				best = candidate.second;
				step = candidate.first;
			}
		}

		const Hit& hit = *chosen->first;

		// `step * (clock/384) / 2048` bytes a second - the same arithmetic the decoder's
		// `bytesPerSecond` does.
		const double rate = (double) step * clock / kOutputDivisor / kStepPerByte;

		char name[40] = {};
		std::snprintf (name, sizeof (name), "rf5c_%06X_%u", (unsigned) hit.address,
					   (unsigned) hit.length);

		collector.add (((uint64_t) hit.address << 32) ^ hit.bucket, name, hit.bytes.data (),
					   hit.length, SampleFormat::signMagnitude8, rate);
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
