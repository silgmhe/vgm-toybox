#include "C352Samples.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>

#include "ChipMemoryAssembly.h"
#include "chips/C352Address.h"
#include "SampleCollector.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	constexpr int kVoices = 32;

	/** The C352's sample ROM, as VGM calls it. */
	constexpr uint8_t kRomType = 0x92;

	/** Its whole address space, and the ceiling this needs. */
	constexpr std::size_t kMaxRomBytes = 32u << 20;

	/** Bit 14 of a voice's flags: set by the driver, acted on by the execute write. */
	constexpr uint16_t kFlagKeyOn = 0x4000;

	/** Bytes are logarithmically coded - a level per byte would be a shape the chip cannot make. */
	constexpr uint16_t kFlagMuLaw = 0x0008;

	/** Not a recording at all: this voice feeds back a shift register. */
	constexpr uint16_t kFlagNoise = 0x0010;

	/** Bytes per second per unit of the frequency register (`kClockDivisor` in the decoder). */
	constexpr double kBytesPerStepDivisor = 18874368.0;

	/** The rate at frequency 0x8000, used only when a voice never wrote its frequency register. */
	constexpr double kRootRateDivisor = 576.0;
	constexpr double kFallbackClock = 12288000.0;

	struct Voice
	{
		uint16_t flags = 0;
		uint32_t frequency = 0;
		/** The frequency register at the moment the key-on bit was set, which is what the voice starts
			at; a slide changes the register afterwards and the execute reads the changed value. */
		uint32_t keyOnFrequency = 0;
		uint32_t bank = 0;
		uint32_t start = 0;
		uint32_t end = 0;
	};

	/** One voice starting one sample, before de-duplication. */
	struct Hit
	{
		uint32_t address = 0;
		std::size_t length = 0;
		uint32_t frequency = 0;
	};
}

std::vector<playback::DecodedSample> collectC352Samples (const midi::SongIr& ir)
{
	SampleCollector collector;

	// `clipToContent` false: the C352's blocks declare the size of the chip's memory, and an image
	// shorter than that is normal - the addresses beyond it simply read nothing.
	const std::vector<uint8_t> rom = memory::rom (ir, kRomType, kMaxRomBytes, false);

	if (rom.empty ())
	{
		return collector.take ();
	}

	double clock = kFallbackClock;

	for (const auto& slot : ir.chips)
	{
		if (slot.kind == midi::ChipKind::c352 && slot.clockHz != 0)
		{
			clock = (double) slot.clockHz;
		}
	}

	const double bytesPerStep = clock / kBytesPerStepDivisor;
	const double referenceRate = clock / kRootRateDivisor;

	Voice voices[kVoices];

	// Collected first, decided afterwards.
	//
	// One sample is often played by several voices at several pitch registers - a drum tuned per
	// hit, a vocal phrase bent up and down - and the rate a file should declare is not whichever
	// voice happened to touch it first. The lowest of them is a bent-down note, and writing the file
	// at that rate makes it play back slow and an octave low; that is exactly what an earlier version
	// did, and it was heard.
	//
	// The highest is the closest thing to the pitch the sample was recorded at, because pitch
	// registers bend downward far more often than up. It is an inference, but a defensible one, and
	// it beats "whichever came first" by not depending on the order of a stream.
	std::vector<Hit> hits;

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::c352)
		{
			continue;
		}

		// The command carries a sixteen-bit offset; the decoder reads it the same way.
		const uint16_t offset = (uint16_t) (((uint16_t) (write.port & 0x7F) << 8) | write.reg);

		// Anything at or above 0x100 is the chip's own control space, not a voice register.
		if (offset >= 0x100)
		{
			if (offset != 0x202)
			{
				continue;
			}

			// The execute write: this is where a key-on becomes a voice actually starting, and so
			// where the sample it names becomes part of the song.
			for (int i = 0; i < kVoices; ++i)
			{
				const Voice& voice = voices[i];

				if ((voice.flags & kFlagKeyOn) == 0)
				{
					continue;
				}

				// The bit stays set until the core clears it on its own sweep; clearing it here
				// keeps a second execute from collecting the same voice again.
				voices[i].flags = (uint16_t) (voice.flags & ~kFlagKeyOn);

				// Noise is not a recording, so it is skipped. mu-law also is one, and the display
				// now decodes it with the decoder's own `muLaw[]` table - but this collector still
				// skips it, because that table is private to `C352Decoder`.
				//
				// Known inconsistency, written down rather than left to be discovered: export a
				// mu-law voice and the folder is missing a file the row can draw. The fix is to move
				// `buildMuLawTable` into a shared header and give `SampleFormat` a `mulaw8` value;
				// it is not done here because it touches both call sites and neither is broken.
				if ((voice.flags & (kFlagMuLaw | kFlagNoise)) != 0)
				{
					continue;
				}

				const uint32_t address = (voice.bank << 16) | voice.start;

				if (address >= rom.size ())
				{
					continue;
				}

				// `wave_end` is played: the core fetches the byte and only then checks for the end,
				// so the region is inclusive on both sides.
				// A wrapped sample is as long as the window allows - see `chips/C352Address.h`.
				const std::size_t length = (std::min) (chips::c352SampleLength (voice.start, voice.end),
														  rom.size () - address);

				hits.push_back (Hit { address, length, voice.keyOnFrequency > 0 ? voice.keyOnFrequency : voice.frequency });
			}

			continue;
		}

		const int index = offset >> 3;

		if (index < 0 || index >= kVoices)
		{
			continue;
		}

		Voice& voice = voices[index];

		switch (offset & 7)
		{
		case 2: voice.frequency = write.value; break;
		case 3:
			voice.flags = (uint16_t) write.value;

			// The bit arriving is the voice starting, so this is the pitch it starts at.
			if ((voice.flags & kFlagKeyOn) != 0)
			{
				voice.keyOnFrequency = voice.frequency;
			}

			break;
		case 4: voice.bank = write.value; break;
		case 5: voice.start = write.value; break;
		case 6: voice.end = write.value; break;
		default: break;
		}
	}

	std::map<uint64_t, uint32_t> highestFrequency;

	for (const auto& hit : hits)
	{
		const uint64_t key = ((uint64_t) hit.address << 32) | hit.length;
		auto& highest = highestFrequency[key];

		highest = (std::max) (highest, hit.frequency);
	}

	for (const auto& hit : hits)
	{
		const uint64_t key = ((uint64_t) hit.address << 32) | hit.length;
		const uint32_t frequency = highestFrequency[key];
		const double rate = (frequency > 0) ? (double) frequency * bytesPerStep : referenceRate;

		char name[32] = {};
		std::snprintf (name, sizeof (name), "c352_%06X_%u", (unsigned) hit.address,
					   (unsigned) hit.length);

		collector.add (key, name, rom.data () + hit.address, hit.length,
					   SampleFormat::pcm8Signed, rate);
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
