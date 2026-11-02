#include "K053260Samples.h"

#include <algorithm>
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
	constexpr int kVoices = 4;

	/** Voice *n*'s eight registers start here (`kChannelBlockBase` in the decoder). */
	constexpr uint8_t kChannelBlockBase = 0x08;
	constexpr uint8_t kChannelBlockSize = 8;
	constexpr uint8_t kRegisterOffsetMask = 0x3F;

	/** The K053260's sample ROM, as VGM calls it (`K053260Decoder::kRequirements`). */
	constexpr uint8_t kRomType = 0x8E;

	/** Twenty-one address bits, and the ceiling this needs. */
	constexpr std::size_t kMaxRomBytes = 2u << 20;

	constexpr double kFallbackClock = 3579545.0;

	/** `pitch / 4096` bytes per output sample at `clock / 64` samples a second. */
	constexpr double kPitchDivisor = 64.0 * 4096.0;

	struct Voice
	{
		uint32_t pitch = 0;
		uint32_t length = 0;
		uint32_t start = 0;
		bool loop = false;
		bool kadpcm = false;

		/** Set while the key-on mask has this voice's bit, so one press is one sample: the drivers
			re-write the same mask once per voice, and every one of those writes would otherwise be
			a second hit on the same bytes. */
		bool keyed = false;
	};

	/** One voice starting one sample, before de-duplication. */
	struct Hit
	{
		uint32_t start = 0;
		std::size_t length = 0;
		uint32_t pitch = 0;
	};
}

std::vector<playback::DecodedSample> collectK053260Samples (const midi::SongIr& ir)
{
	SampleCollector collector;

	const std::vector<uint8_t> rom = memory::rom (ir, kRomType, kMaxRomBytes, false);

	if (rom.empty ())
	{
		return collector.take ();
	}

	double clock = kFallbackClock;

	for (const auto& slot : ir.chips)
	{
		if (slot.kind == midi::ChipKind::k053260 && slot.clockHz != 0)
		{
			clock = (double) slot.clockHz;
		}
	}

	Voice voices[kVoices];

	// Collected first, decided afterwards: one sample is often played by several voices at several
	// pitches - a drum tuned per hit - and the rate a file declares is the highest of them, because
	// pitch registers bend downward far more often than up.
	std::vector<Hit> hits;

	const auto collect = [&] (const Voice& voice)
	{
		// KADPCM is four-bit deltas; `SampleFormat` has no value for it, and handing the packed
		// bytes to an eight-bit decoder would produce noise that looks like a waveform.
		if (voice.kadpcm || voice.length <= 1)
		{
			return;
		}

		// The core's first fetch is the byte after the start address (`KDSC_play` pre-increments),
		// so the region is `start + 1` for `length` bytes.
		const uint32_t begin = voice.start + 1;

		if (begin >= rom.size ())
		{
			return;
		}

		const std::size_t length = (std::min) ((std::size_t) voice.length, rom.size () - begin);

		if (length <= 1)
		{
			return;
		}

		hits.push_back (Hit { voice.start, length, voice.pitch });
	};

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::k053260)
		{
			continue;
		}

		const uint8_t offset = (uint8_t) (write.reg & kRegisterOffsetMask);
		const uint16_t value = write.value;

		// The key-on mask is the only thing that starts a voice, and only on a rising edge. Its
		// high nibble is the per-voice reverse flag, which does not change which bytes a sample is.
		if (offset == 0x28)
		{
			for (int i = 0; i < kVoices; ++i)
			{
				const bool on = (value & (1u << i)) != 0;

				if (on && ! voices[i].keyed)
				{
					collect (voices[i]);
				}

				voices[i].keyed = on;
			}

			continue;
		}

		// Loop is the low nibble here, KADPCM the high one - two facts in one register, a nibble
		// apart, which is the kind of pairing that survives a wrong mask by looking plausible.
		if (offset == 0x2A)
		{
			for (int i = 0; i < kVoices; ++i)
			{
				voices[i].loop = (value & (1u << i)) != 0;
				voices[i].kadpcm = (value & (1u << (i + 4))) != 0;
			}

			continue;
		}

		if (offset < kChannelBlockBase
			|| offset >= kChannelBlockBase + kVoices * kChannelBlockSize)
		{
			continue;
		}

		const int index = (int) ((offset - kChannelBlockBase) / kChannelBlockSize);
		Voice& voice = voices[index];

		switch ((offset - kChannelBlockBase) % kChannelBlockSize)
		{
		case 0: voice.pitch = (voice.pitch & 0x0F00u) | value; break;
		case 1: voice.pitch = (voice.pitch & 0x00FFu) | ((uint32_t) (value & 0x0F) << 8); break;
		case 2: voice.length = (voice.length & 0xFF00u) | value; break;
		case 3: voice.length = (voice.length & 0x00FFu) | ((uint32_t) value << 8); break;
		case 4: voice.start = (voice.start & 0x1FFF00u) | value; break;
		case 5: voice.start = (voice.start & 0x1F00FFu) | ((uint32_t) value << 8); break;
		case 6: voice.start = (voice.start & 0x00FFFFu) | ((uint32_t) (value & 0x1F) << 16); break;
		default: break;
		}
	}

	std::map<uint64_t, uint32_t> highestPitch;

	for (const auto& hit : hits)
	{
		const uint64_t key = ((uint64_t) hit.start << 32) | hit.length;
		auto& highest = highestPitch[key];

		highest = (std::max) (highest, hit.pitch);
	}

	for (const auto& hit : hits)
	{
		const uint64_t key = ((uint64_t) hit.start << 32) | hit.length;
		const uint32_t pitch = highestPitch[key];
		const double rate = (double) pitch * clock / kPitchDivisor;
		const uint32_t begin = hit.start + 1;

		char name[32] = {};
		std::snprintf (name, sizeof (name), "k053260_%06X_%u", (unsigned) begin,
					   (unsigned) hit.length);

		collector.add (key, name, rom.data () + begin, hit.length,
					   SampleFormat::pcm8Signed, rate);
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
