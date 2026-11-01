#include "C352Samples.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "ChipMemoryAssembly.h"
#include "SampleCollector.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{
namespace
{
	constexpr int kVoices = 32;

	/** Eight registers per voice, one offset each: two volumes, frequency, flags, bank, start, end,
		loop. The offsets are the low three bits of the register number. */
	constexpr int kRegistersPerVoice = 8;

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
		uint32_t bank = 0;
		uint32_t start = 0;
		uint32_t end = 0;
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

				if ((voice.flags & (kFlagMuLaw | kFlagNoise)) != 0 || voice.end < voice.start)
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
				const std::size_t length =
					(std::min) ((std::size_t) (voice.end - voice.start) + 1, rom.size () - address);

				// The rate this voice reads its bytes at - not the chip's reference rate. A voice
				// an octave below the reference reads at half speed, and writing the file at the
				// reference would make it play back twice as fast and an octave up.
				const double rate = (voice.frequency > 0) ? (double) voice.frequency * bytesPerStep
														  : referenceRate;

				char name[32] = {};
				std::snprintf (name, sizeof (name), "c352_%06X_%u", (unsigned) address,
							   (unsigned) length);

				collector.add (((uint64_t) address << 32) | length, name, rom.data () + address,
							   length, SampleFormat::pcm8Signed, rate);
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
		case 3: voice.flags = (uint16_t) write.value; break;
		case 4: voice.bank = write.value; break;
		case 5: voice.start = write.value; break;
		case 6: voice.end = write.value; break;
		default: break;
		}
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
