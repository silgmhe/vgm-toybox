#include "Rf5c68Samples.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "ChipMemoryAssembly.h"
#include "SampleCollector.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	constexpr int kChannels = 8;

	/** The RAM the chip addresses, sixteen bits wide - the same ceiling the decoder uses. */
	constexpr std::size_t kRamBytes = 64 * 1024;

	/** A PCM bank in VGM: blocks below 0x40 concatenate. */
	constexpr uint8_t kPcmBankType = 0x02;

	/** A RAM page in VGM: 0xC0-0xDF carry a 16-bit address and the bytes that belong there. */
	constexpr uint8_t kRamBlockType = 0xC1;

	constexpr double kFallbackClock = 12500000.0;

	/** The chip runs at `clock / 384` samples a second. */
	constexpr double kOutputDivisor = 384.0;

	/** A step of 2048 advances the play head one byte per chip sample. */
	constexpr double kStepPerByte = 2048.0;

	/** `sampleTime`'s unit, which is what turns the stream clock into seconds. */
	constexpr double kStreamRate = 44100.0;

	/** A note longer than this is a driver mistake or a lost key-off; stop counting. */
	constexpr std::size_t kMaxNoteBytes = 64 * 1024;

	/** How long a release may last and still belong to the same note.

		The driver rewrites the whole `0x08` mask between notes and drops a voice's bit on the way, so
		one note arrives as several on/off pairs. Twenty milliseconds is longer than that rewrite and
		shorter than the gap between two notes of one channel in these rips. Without it a song became
		1629 files, one per fragment.
	*/
	constexpr double kContinuation = 0.020;

	/** Notes are compared by length rounded up to this, so a few bytes of jitter do not make a file. */
	constexpr std::size_t kLengthBucket = 128;

	/** One address holds different bytes at different moments, so a sample is a **sequence**, not a
		range: the head walks the RAM as the pages land under it.

		This is the model the simulation in `build/rf5c_sim.py` reproduces, and the one the chip
		implements: `scd_pcm.c` reads `RAM[Addr]` and advances `Addr` by `Step` for every output
		sample. An export that copies a range at any single moment cannot match it - the range holds
		whatever was written last, while the chip heard the earlier contents first.
	*/
	struct Voice
	{
		bool on = false;
		uint8_t startPage = 0;
		uint16_t step = 0;

		bool playing = false;
		bool released = false;
		uint16_t noteStep = 0;			// the step the note started at: a file has one rate
		uint32_t releasedAt = 0;
		double head = 0.0;				// byte position in RAM
		uint32_t advancedAt = 0;		// stream sample the head was last moved to
		std::vector<uint8_t> bytes;		// what it has read since the key-on
	};

	uint64_t hashOf (const std::vector<uint8_t>& bytes)
	{
		uint64_t hash = 1469598103934665603ull;

		for (const uint8_t byte : bytes)
		{
			hash ^= (uint64_t) byte;
			hash *= 1099511628211ull;
		}

		return hash;
	}
}

std::vector<playback::DecodedSample> collectRf5cSamples (const midi::SongIr& ir)
{
	SampleCollector collector;

	// The load-time image: `0x67` blocks of type 0x02, in file order.
	std::vector<uint8_t> ram = memory::pcmBank (ir, kPcmBankType, kRamBytes);

	if (ram.empty ())
	{
		return collector.take ();
	}

	/** **The whole bank**, because a `0x68` copy names a position inside it and this rip's bank is
		155 KB for a 64 KB memory - `pcmBank` cannot give the part past the first 64 KB. */
	std::vector<uint8_t> bankBytes;

	for (const auto& block : ir.roms)
	{
		if (block.memoryType == kPcmBankType && block.data != nullptr)
		{
			bankBytes.insert (bankBytes.end (), block.data, block.data + block.size);
		}
	}

	std::vector<const midi::RomBlock*> ramWrites;
	std::vector<const midi::RamCopy*> copies;

	for (const auto& block : ir.roms)
	{
		if (block.memoryType == kRamBlockType)
		{
			ramWrites.push_back (&block);
		}
	}

	for (const auto& copy : ir.ramCopies)
	{
		if (copy.bankType == kPcmBankType)
		{
			copies.push_back (&copy);
		}
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
	uint8_t selected = 0;			// the channel `0x07` selected, for the low registers
	uint8_t ramBank = 0;			// the bank every memory address is or-ed with

	std::vector<playback::DecodedSample> done;
	std::map<uint64_t, bool> seen;
	std::size_t emitted = 0;

	/** Moves every sounding head to `now`, collecting what it passes.

		The rate is `step * clock / 384 / 2048` bytes a second, and the head stops at a `0xFF` byte
		because that is where the chip jumps to the loop address - what follows is the sustain, not
		the sample.
	*/
	const auto advanceTo = [&] (uint32_t now)
	{
		for (int i = 0; i < kChannels; ++i)
		{
			Voice& voice = voices[i];

			if (! voice.playing || voice.step == 0)
			{
				voice.advancedAt = now;
				continue;
			}

			if (now > voice.advancedAt)
			{
				const double rate = (double) voice.step * clock / kOutputDivisor / kStepPerByte;
				const std::size_t wanted =
					(std::size_t) ((double) (now - voice.advancedAt) * rate / kStreamRate);

				for (std::size_t n = 0; n < wanted && voice.bytes.size () < kMaxNoteBytes; ++n)
				{
					const uint8_t byte = ram[(std::size_t) voice.head & (kRamBytes - 1)];

					if (byte == 0xFF)
					{
						voice.playing = false;
						break;
					}

					voice.bytes.push_back (byte);
					voice.head += (double) voice.step / kStepPerByte;
				}
			}

			voice.advancedAt = now;
		}
	};

	/** Files one note, if it read anything a listener would call a sample. */
	const auto finish = [&] (Voice& voice)
	{
		const uint16_t step = (voice.noteStep != 0) ? voice.noteStep : voice.step;

		if (voice.bytes.size () < 2 || step == 0)
		{
			voice.bytes.clear ();
			return;
		}

		const uint32_t start = (uint32_t) voice.startPage << 8;

		// **One file per instrument, not per press.** Two notes of one instrument differ by a few
		// bytes of gate time and by whatever silence preceded them, so exact lengths and exact
		// contents make every note a file; a length bucket collapses them.
		const std::size_t bucket =
			((voice.bytes.size () + kLengthBucket - 1) / kLengthBucket) * kLengthBucket;

		if (! seen.insert (std::make_pair (start, bucket)).second)
		{
			voice.bytes.clear ();
			return;
		}

		const uint64_t hash = hashOf (voice.bytes);

		const double rate = (double) step * clock / kOutputDivisor / kStepPerByte;

		char name[48] = {};
		std::snprintf (name, sizeof (name), "rf5c_%06X_%u", (unsigned) start,
					   (unsigned) voice.bytes.size ());

		collector.add (hash, name, voice.bytes.data (), voice.bytes.size (),
					   SampleFormat::signMagnitude8, rate);

		++emitted;
		voice.bytes.clear ();
	};

	std::size_t writeIndex = 0;
	std::size_t blockIndex = 0;
	std::size_t copyIndex = 0;
	uint32_t now = 0;

	// Each lookahead tests its own index first: passing `writes[index].streamOrder` as an argument
	// reads past the end of a finished list, which is a crash rather than an empty answer.
	const auto nextWriteOrder = [&]
	{
		return (writeIndex < ir.writes.size ()) ? ir.writes[writeIndex].streamOrder : UINT32_MAX;
	};

	const auto nextBlockOrder = [&]
	{
		return (blockIndex < ramWrites.size ()) ? ramWrites[blockIndex]->streamOrder : UINT32_MAX;
	};

	const auto nextCopyOrder = [&]
	{
		return (copyIndex < copies.size ()) ? copies[copyIndex]->streamOrder : UINT32_MAX;
	};

	while (writeIndex < ir.writes.size () || blockIndex < ramWrites.size ()
		   || copyIndex < copies.size ())
	{
		const uint32_t nextWrite = nextWriteOrder ();
		const uint32_t nextBlock = nextBlockOrder ();
		const uint32_t nextCopy = nextCopyOrder ();

		// A block carries no clock: it happens between two writes, so the heads are moved to the time
		// of the write that precedes it before its bytes reach the memory.
		if (nextCopy <= nextWrite && nextCopy <= nextBlock)
		{
			const midi::RamCopy& copy = *copies[copyIndex++];
			const std::size_t address = (std::size_t) copy.address | ((std::size_t) ramBank << 12);

			advanceTo (now);

			for (std::size_t i = 0; i < copy.length; ++i)
			{
				if (copy.bankPosition + i < bankBytes.size () && address + i < ram.size ())
				{
					ram[address + i] = bankBytes[copy.bankPosition + i];
				}
			}

			continue;
		}

		if (nextBlock <= nextWrite)
		{
			const midi::RomBlock& block = *ramWrites[blockIndex++];
			const std::size_t address = (std::size_t) block.ramOffset | ((std::size_t) ramBank << 12);

			advanceTo (now);

			for (std::size_t i = 0; i < block.size && address + i < ram.size (); ++i)
			{
				ram[address + i] = block.data[i];
			}

			continue;
		}

		const auto& write = ir.writes[writeIndex++];
		now = write.sampleTime;

		if (write.deviceId >= ir.chips.size ())
		{
			continue;
		}

		const midi::ChipKind kind = ir.chips[write.deviceId].kind;

		if (kind != midi::ChipKind::rf5c68 && kind != midi::ChipKind::rf5c164)
		{
			continue;
		}

		// **Every head moves to this moment before the write changes anything**, which is what keeps
		// a page's bytes from being read for the interval before it arrived.
		advanceTo (now);

		const uint8_t offset = write.reg;
		const uint8_t value = (uint8_t) write.value;

		if (offset == 0x07)
		{
			if ((value & 0x40) != 0)
			{
				selected = (uint8_t) (value & 0x07);
			}
			else
			{
				ramBank = (uint8_t) (value & 0x0F);
			}

			continue;
		}

		// `0x08` is the per-channel mask: a **cleared** bit plays, so clearing one is the key-on and
		// setting it is the key-off (`scd_pcm.c:325-341`).
		if (offset == 0x08)
		{
			for (int i = 0; i < kChannels; ++i)
			{
				const bool on = ((~value >> i) & 1) != 0;
				Voice& voice = voices[i];

				if (on && ! voice.on)
				{
					// **A mask rewrite is not a retrigger.** The driver toggles every voice's bit on
					// its way to writing the next one, so one note arrives as several on/off pairs;
					// only a gap longer than `kContinuation` ends the previous one. Without this the
					// song became 1629 files, one per fragment.
					const bool continued = voice.released
										   && (now - voice.releasedAt)
												  <= (uint32_t) (kContinuation * kStreamRate);

					if (voice.released && ! continued)
					{
						finish (voice);
					}

					if (! continued)
					{
						voice.head = (double) ((uint32_t) voice.startPage << 8);
						voice.bytes.clear ();
						voice.noteStep = voice.step;
					}

					voice.released = false;
					voice.playing = true;
					voice.advancedAt = now;
				}
				else if (! on && voice.on)
				{
					voice.playing = false;
					voice.released = true;
					voice.releasedAt = now;
				}

				voice.on = on;
			}

			continue;
		}

		Voice& voice = voices[selected];

		switch (offset)
		{
		case 0x02: voice.step = (uint16_t) ((voice.step & 0xFF00) | value); break;
		case 0x03: voice.step = (uint16_t) ((voice.step & 0x00FF) | ((uint16_t) value << 8)); break;
		case 0x06: voice.startPage = value; break;
		default: break;
		}
	}

	// Whatever is still playing when the file ends is a note too.
	advanceTo (ir.totalTicks);

	for (int i = 0; i < kChannels; ++i)
	{
		if (voices[i].playing; voices[i].released)
		{
			finish (voices[i]);
		}
	}

	(void) emitted;
	return collector.take ();
}

} // namespace toybox::backends::libvgm
