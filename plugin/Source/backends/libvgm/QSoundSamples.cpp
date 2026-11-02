#include "QSoundSamples.h"

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
	/** PCM voices, and only these.

		The three ADPCM voices are a separate register block (`0xCA`-`0xD5`, started by `0xD6+n`) in a
		different format - one byte carries two four-bit deltas, not one level - so exporting them
		needs `SampleFormat` to grow a value and a decoder. That part is a known amount of work, not
		a reason to skip it.

		What is a reason: there is no song to check the result against. On `01 QSound Logo`, the one
		QSound file in `test_file`, the drivers write the ADPCM parameter registers five times and
		start a voice zero times. A decoder written without a file that plays it can be shown to
		compile and to not crash; it cannot be shown to be right, and this branch has already spent
		eight guesses on a chip whose data was there all along (see
		`docs/memo/2026-11-02-eight-guesses-before-a-wrapped-window.md`). Left as a written-down debt:
		the design document's §9.3 has the conditions for doing it.
	*/
	constexpr int kVoices = 16;

	/** The ADPCM voices, whose registers sit in their own block above the PCM ones. */
	constexpr int kAdpcmVoices = 3;

	/** The QSound's sample ROM, as VGM calls it (`QSoundDecoder::kRequirements`). */
	constexpr uint8_t kRomType = 0x8F;

	/** The chip's address space: fifteen bank bits above a sixteen-bit address. */
	constexpr std::size_t kMaxRomBytes = 8u << 20;

	constexpr double kFallbackClock = 60000000.0;

	/** `rate` is 4.12 fixed point over the chip's own sample clock, which is `clock / 2 / 1248`. */
	constexpr double kRateDivisor = 4096.0 * 2.0 * 1248.0;
	constexpr double kOutputDivisor = 2.0 * 1248.0;

	/** The ADPCM voices are updated one at a time, so each runs at a third of the output rate. */
	constexpr double kAdpcmClockDivisor = kOutputDivisor * 3.0;

	/** Bit 15 of a bank: the samples live above the DSP program ROM, and the core refuses a bank
		without it. */
	constexpr uint16_t kSampleBankFlag = 0x8000;
	constexpr uint16_t kBankIndexMask = 0x7FFF;
	constexpr int kBankToByteShift = 16;

	/** How many bytes `start..end` covers inside one sixty-four-kilobyte bank.

		The addresses are unsigned even though `end` is stored signed, and a sample that starts near
		the top of its bank wraps through zero back to the bottom - Street Fighter II's voices do
		exactly that. `end == start` is a sample of no length rather than a whole bank.
	*/
	uint32_t bankLength (uint32_t start, uint32_t end)
	{
		start &= 0xFFFF;
		end &= 0xFFFF;

		if (end == start)
		{
			return 0;
		}

		return (end > start) ? (end - start) : (0x10000u - start + end);
	}

	struct Voice
	{
		uint32_t bank = 0;
		uint32_t rate = 0;
		uint32_t volume = 0;
		uint32_t start = 0;
		uint32_t end = 0;

		/** Set while the voice is sounding, so one note is one sample: the drivers rewrite `rate`
			several times per note for slides, and every one of those writes would otherwise be a
			new hit on the same bytes. */
		bool keyed = false;
	};

	/** The three ADPCM voices: four registers each from 0xCA, and a start flag at 0xD6+n.

		`bank` carries the same bit 15 gate the PCM voices use - the samples live above the DSP
		program, and the core returns silence for a bank without it.
	*/
	struct AdpcmVoice
	{
		uint32_t start = 0;
		uint32_t end = 0;
		uint32_t bank = 0;
		uint32_t volume = 0;

		/** Set while the start flag is non-zero, so one write of it is one sample. */
		bool keyed = false;
	};

	/** One voice starting one sample, before de-duplication. */
	struct Hit
	{
		uint32_t address = 0;
		std::size_t length = 0;
		uint32_t rate = 0;
		bool adpcm = false;
	};
}

std::vector<playback::DecodedSample> collectQSoundSamples (const midi::SongIr& ir)
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
		if (slot.kind == midi::ChipKind::qsound && slot.clockHz != 0)
		{
			clock = (double) slot.clockHz;
		}
	}

	const double sampleRate = clock / kOutputDivisor;

	Voice voices[kVoices];
	AdpcmVoice adpcm[kAdpcmVoices];

	// Collected first, decided afterwards, for the same reason the C352 collector does it: one
	// sample is often played by several voices at several rates - a drum tuned per hit - and the
	// rate a file should declare is the highest of them, because rate registers bend downward far
	// more often than up. Taking the first would make the file depend on the order of a stream.
	std::vector<Hit> hits;

	/** Records a sample if this voice has just started one. */
	const auto collect = [&] (const Voice& voice)
	{
		if (voice.rate == 0 || voice.volume == 0 || (voice.bank & kSampleBankFlag) == 0)
		{
			return;
		}

		const uint32_t address = ((voice.bank & kBankIndexMask) << kBankToByteShift)
							   | (voice.start & 0xFFFF);
		const std::size_t length = bankLength (voice.start, voice.end);

		if (length <= 1 || address >= rom.size ())
		{
			return;
		}

		hits.push_back (Hit { address, length, voice.rate, false });
	};

	/** Records an ADPCM sample when its start flag is written. */
	const auto collectAdpcm = [&] (const AdpcmVoice& voice)
	{
		if ((voice.bank & kSampleBankFlag) == 0 || voice.end <= voice.start)
		{
			return;
		}

		const uint32_t address = ((voice.bank & kBankIndexMask) << kBankToByteShift)
							   | (voice.start & 0xFFFF);
		const std::size_t length = (std::size_t) (voice.end - voice.start);

		if (length <= 1 || address >= rom.size ())
		{
			return;
		}

		hits.push_back (Hit { address, (std::min) (length, rom.size () - address),
							  (uint32_t) clock, true });
	};

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::qsound)
		{
			continue;
		}

		// QSound's command carries its register offset in one byte, which the reader stores in
		// `reg` - there is no page half, unlike the C352's sixteen-bit offsets.
		const uint8_t offset = write.reg;

		// The three ADPCM voices: four registers each from 0xCA, and a start flag one byte apart
		// from 0xD6. The flag is what starts one, and only its rising edge counts - the core
		// consumes it on the next sample it processes.
		if (offset >= 0xCA && offset <= 0xD5)
		{
			const int index = (int) ((offset - 0xCA) / 4);

			if (index < kAdpcmVoices)
			{
				AdpcmVoice& voice = adpcm[index];

				switch ((offset - 0xCA) % 4)
				{
				case 0: voice.start = write.value; break;
				case 1: voice.end = write.value; break;
				case 2: voice.bank = write.value; break;
				case 3: voice.volume = write.value; break;
				default: break;
				}
			}

			continue;
		}

		if (offset >= 0xD6 && offset <= 0xD8)
		{
			const int index = (int) (offset - 0xD6);

			if (index < kAdpcmVoices)
			{
				if (write.value != 0 && ! adpcm[index].keyed)
				{
					collectAdpcm (adpcm[index]);
				}

				adpcm[index].keyed = (write.value != 0);
			}

			continue;
		}

		if (offset >= 0x80)
		{
			continue;
		}

		const int index = (int) (offset >> 3);

		if (index < 0 || index >= kVoices)
		{
			continue;
		}

		Voice& voice = voices[index];

		switch (offset & 0x07)
		{
		case 0x00:
			// The bank register belongs to the NEXT voice (`init_register_map` in the core), and a
			// driver that wrote its own would give every voice the previous one's bank - an
			// address that is plausible, in range, and the wrong bytes.
			voices[(index + 1) % kVoices].bank = write.value;
			break;

		case 0x01:
			// The start address, and also the play position: the core advances this register as it
			// plays, so the value here is the one the driver just wrote, which is where it starts.
			voice.start = write.value;
			break;

		case 0x02:
			voice.rate = write.value;

			if (voice.rate == 0)
			{
				voice.keyed = false;
			}
			else if (! voice.keyed)
			{
				collect (voice);
				voice.keyed = (voice.volume != 0);
			}

			break;

		case 0x05:
			voice.end = write.value;
			break;

		case 0x06:
			voice.volume = write.value;

			if (voice.volume == 0)
			{
				voice.keyed = false;
			}
			else if (! voice.keyed)
			{
				collect (voice);
				voice.keyed = (voice.rate != 0);
			}

			break;

		default:
			// phase, loop length, pan and echo do not change which bytes a sample is.
			break;
		}
	}

	std::map<uint64_t, uint32_t> highestRate;

	for (const auto& hit : hits)
	{
		const uint64_t key = ((uint64_t) hit.address << 32) | ((uint64_t) hit.length << 1) | (hit.adpcm ? 1 : 0);
		auto& highest = highestRate[key];

		highest = (std::max) (highest, hit.rate);
	}

	for (const auto& hit : hits)
	{
		const uint64_t key = ((uint64_t) hit.address << 32) | ((uint64_t) hit.length << 1) | (hit.adpcm ? 1 : 0);
		const uint32_t rateRegister = highestRate[key];

		// An ADPCM voice has no pitch register: it runs at a third of the output rate, which is why
		// the clock travels on the hit rather than a register value.
		const double rate = hit.adpcm
			? (double) hit.rate / kAdpcmClockDivisor
			: (double) rateRegister / 4096.0 * sampleRate;

		char name[32] = {};
		std::snprintf (name, sizeof (name), "qsound_%s%06X_%u", hit.adpcm ? "adpcm_" : "",
					   (unsigned) hit.address, (unsigned) hit.length);

		collector.add (key, name, rom.data () + hit.address, hit.length,
					   hit.adpcm ? SampleFormat::qsoundAdpcm : SampleFormat::pcm8Signed, rate);
	}

	return collector.take ();
}

} // namespace toybox::backends::libvgm
