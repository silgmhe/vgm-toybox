#include "QSoundDecoder.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <vector>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	// The VGM header stores the QSound's old 4 MHz clock and libvgm multiplies it by 15 before the
	// core sees it (vgmplayer.cpp, "QSound clock used to be 4 MHz"), so the normalised clock is
	// 60 MHz and the chip renders at 60 MHz / 2 / 1248 = 24.04 kHz.
	constexpr std::uint32_t kDefaultClock = 60000000;
	constexpr std::uint32_t kLegacyClockCeiling = 5000000;		// below this the header is the old clock
	constexpr std::uint32_t kLegacyClockScale = 15;

	// pcm_update() adds `rate` (4.12 fixed point) to a 28-bit position each output sample, so a
	// voice advances by `rate / 4096` bytes per sample and `rate * clock / (4096 * 2 * 1248)` a
	// second.
	constexpr double kRateDivisor = 4096.0 * 2.0 * 1248.0;

	// The chip renders at clock / 2 / 1248.
	constexpr double kOutputDivisor = 2.0 * 1248.0;

	// The three ADPCM voices share the renderer: one of them is serviced every third sample with a
	// single nibble, so each channel runs at 8 kHz.
	constexpr double kAdpcmClockDivisor = kOutputDivisor * 3.0;

	// The end of a voice that loops is the driver's business, not the sample's: a looping note only
	// stops when the rate or the volume goes to zero, like every other sampled player here.
	constexpr int kHighestMelodicNote = 108;		// C8: above this a "period" is an artifact
	constexpr std::size_t kMeasurementUnits = 8192;

	// adpcm_step_table from qsound_ctr.c, indexed by 8 + the signed nibble.
	constexpr int kAdpcmStepTable[16] = { 154, 154, 128, 102, 77, 58, 58, 58,
										  58, 58, 58, 58, 77, 102, 128, 154 };

	constexpr std::size_t kPcmVoices = 16;
	constexpr std::size_t kAdpcmVoices = 3;

	/** One note table for every voice. The PCM voices turn their 4.12 rate into bytes a second, the
		ADPCM voices have no rate at all - their nibble clock is fixed - so the table takes a fixed
		rate for them instead of the period register. */
	class QSoundNoteTable final : public NoteTable
	{
	public:
		QSoundNoteTable (double clock, double fixedRate, const double* sampleCycle,
						 const bool* melodic, const std::vector<std::uint8_t>* rom)
			: unitsPerSecondPerStep (fixedRate > 0.0 ? 0.0 : clock / kRateDivisor)
			, fixedRate (fixedRate)
			, rootRate (clock / kOutputDivisor)
			, cycleLength (sampleCycle)
			, melodic (melodic)
			, rom (rom) {}

		int noteFor (std::uint32_t period, int, int* finePitch) const override
		{
			const double rate = (fixedRate > 0.0) ? fixedRate
												  : (double) period * unitsPerSecondPerStep;

			if (melodic != nullptr && *melodic && cycleLength != nullptr && *cycleLength > 0.0)
			{
				// L1: the transposition the rate register asks for, never the sample's bytes
				return quantisePlaybackRate (rate, rootRate, finePitch);
			}

			if (rom == nullptr || rom->empty())
			{
				// nothing to measure: the register against the chip's own rate, with one byte per
				// output sample as middle C
				return quantisePlaybackRate (rate, rootRate, finePitch);
			}

			if (finePitch != nullptr)
			{
				*finePitch = 0;
			}

			return 60;		// a drum: the one-shot trigger, not a pitch
		}

	private:
		double unitsPerSecondPerStep;
		double fixedRate;
		double rootRate;
		const double* cycleLength;
		const bool* melodic;
		const std::vector<std::uint8_t>* rom;
	};

	/** What a measurement belongs to: the registers that name one sample's bytes. */
	struct SampleKey
	{
		std::uint32_t address;
		std::uint32_t length;
		std::int32_t loopOffset;

		bool operator< (const SampleKey& other) const
		{
			return std::tie (address, length, loopOffset) < std::tie (other.address, other.length,
																	 other.loopOffset);
		}
	};

	std::uint32_t pow2Mask (std::size_t size)
	{
		if (size == 0)
		{
			return 0;
		}

		std::uint32_t mask = (std::uint32_t) (size - 1);
		mask |= mask >> 1;
		mask |= mask >> 2;
		mask |= mask >> 4;
		mask |= mask >> 8;
		mask |= mask >> 16;
		return mask;
	}
}

struct QSoundDecoder::Impl
{
	struct PcmVoice
	{
		std::uint16_t bank = 0x8000;
		std::uint16_t addr = 0;			// current position, and the start a note latches
		std::uint16_t phase = 0;
		std::uint16_t rate = 0;			// 4.12 fixed point
		std::int16_t loopLength = 0;
		std::int16_t end = 0;
		std::int16_t volume = 0;
		std::int16_t echo = 0;
		std::uint16_t pan = 0x120;

		// latched at the rising edge that starts a note
		bool keyed = false;
		std::uint32_t latchStart = 0;
		std::uint32_t gateEnd = 0;
		double cycleLength = 0.0;
		bool melodic = false;
		std::uint32_t keyOns = 0;
	};

	struct AdpcmVoice
	{
		std::uint16_t startAddress = 0;
		std::uint16_t endAddress = 0;
		std::uint16_t bank = 0x8000;
		std::int16_t volume = 0;
		std::uint16_t flag = 0;
		std::uint16_t pan = 0x120;

		bool keyed = false;
		std::uint32_t gateEnd = 0;
		std::uint32_t latchStart = 0;
		std::uint32_t latchEnd = 0;
		double cycleLength = 0.0;
		bool melodic = false;
		std::uint32_t keyOns = 0;
	};

	PcmVoice pcm[kPcmVoices];
	AdpcmVoice adpcm[kAdpcmVoices];
	std::uint32_t now = 0;
	double clock = (double) kDefaultClock;
	double streamRate = 44100.0;
	std::vector<std::uint8_t> rom;
	std::uint32_t romMask = 0;
	std::map<SampleKey, double> cycleCache;
	std::vector<std::unique_ptr<QSoundNoteTable>> tables;

	std::uint8_t romByte (std::uint32_t address) const
	{
		const std::uint32_t index = address & romMask;
		return (index < rom.size()) ? rom[index] : 0;
	}

	/** The chip masks a banked address to the ROM's power-of-two size and refuses to read the DSP
		program ROM (bank bit 15 clear). */
	int sampleAt (std::uint16_t bank, std::uint16_t address) const
	{
		if (rom.empty() || (bank & 0x8000) == 0)
		{
			return 0;
		}

		return (int) (std::int8_t) romByte ((((std::uint32_t) (bank & 0x7FFF)) << 16) | address);
	}

	double unitsPerSecond (const PcmVoice& voice) const
	{
		return (double) voice.rate * clock / kRateDivisor;
	}

	double adpcmUnitsPerSecond() const
	{
		return clock / kAdpcmClockDivisor;
	}

	std::uint32_t pcmAddressOf (const PcmVoice& voice) const
	{
		return (((std::uint32_t) (voice.bank & 0x7FFF)) << 16) | voice.addr;
	}

	/** The bytes a sample covers inside its 64 KB bank.

		`end` is stored as a signed 16-bit register, but the address is unsigned - and a sample that
		starts near the top of its bank wraps through 0x10000 back to the bottom (Street Fighter II's
		voices do exactly that), which as a signed comparison looks like an empty sample. */
	static std::uint32_t bankLength (std::uint32_t start, std::uint32_t end)
	{
		start &= 0xFFFF;
		end &= 0xFFFF;

		if (end == start)
		{
			return 0;
		}

		return (end > start) ? (end - start) : (0x10000u - start + end);
	}

	/** How long a non-looping note lasts: the sample's own length over the rate the driver set. A
		looping voice has no such length - only the rate going to zero ends it. */
	std::uint32_t durationOf (const PcmVoice& voice) const
	{
		if (voice.rate == 0 || voice.loopLength != 0)
		{
			return 0;
		}

		// the chip's own samples, converted to the rate the extractor counts in
		const double bytes = (double) bankLength (voice.latchStart, (std::uint16_t) voice.end);
		const double chipSamples = bytes / ((double) voice.rate / 4096.0);
		const double samples = chipSamples * streamRate / (clock / kOutputDivisor);
		return (std::uint32_t) std::llround (std::min (samples, 1.0e9));
	}

	/** The cycle length of the sample a voice is about to play - measured once per register set.

		The region runs from the loop start (or the sample start) to the end address; a looping voice
		repeats exactly that, which is what a sustained note should be tuned to. */
	double cycleFor (const PcmVoice& voice)
	{
		if (rom.empty() || voice.rate == 0)
		{
			return 0.0;
		}

		const std::uint32_t start = voice.latchStart & 0xFFFF;
		const std::uint32_t end = (std::uint16_t) voice.end;
		const std::uint32_t loopLength = (voice.loopLength > 0) ? (std::uint32_t) voice.loopLength : 0;
		const std::uint32_t bank = voice.bank;

		const std::uint32_t total = bankLength (start, end);
		std::uint32_t offset = start;
		std::uint32_t length = total;
		if (loopLength > 0 && loopLength <= total)
		{
			offset = (end - loopLength) & 0xFFFF;
			length = loopLength;
		}

		const SampleKey key { ((bank & 0x7FFF) << 16) | offset, length, loopLength > 0 ? 1 : 0 };
		const auto cached = cycleCache.find (key);
		if (cached != cycleCache.end())
		{
			return cached->second;
		}

		const std::size_t window = std::min<std::size_t> ((std::size_t) length, kMeasurementUnits);
		std::vector<double> waveform (window);
		for (std::size_t i = 0; i < window; ++i)
		{
			waveform[i] = (double) sampleAt (bank, (std::uint16_t) (offset + (std::uint32_t) i));
		}

		const double cycle = estimateCycleLength (waveform, 0, loopLength > 0 ? waveform.size() : 0);
		cycleCache[key] = cycle;
		return cycle;
	}

	/** The same for an ADPCM voice: its nibbles are deltas, so they are summed the way the core's
		`adpcm_update()` sums them before anything can have a period. */
	double adpcmCycleFor (const AdpcmVoice& voice)
	{
		if (rom.empty() || voice.latchEnd <= voice.latchStart)
		{
			return 0.0;
		}

		const SampleKey key { ((voice.bank & 0x7FFF) << 16) | voice.latchStart,
							  voice.latchEnd - voice.latchStart, 0 };
		const auto cached = cycleCache.find (key);
		if (cached != cycleCache.end())
		{
			return cached->second;
		}

		const std::size_t bytes = std::min<std::size_t> (voice.latchEnd - voice.latchStart,
														 kMeasurementUnits / 2);
		std::vector<double> waveform;
		waveform.reserve (bytes * 2);

		double accumulator = 0.0;
		int stepSize = 10;
		for (std::size_t i = 0; i < bytes; ++i)
		{
			const int byte = sampleAt (voice.bank, (std::uint16_t) (voice.latchStart + i));
			for (int half = 0; half < 2; ++half)
			{
				// top nibble first, then bottom; each is a signed four-bit step
				const int nibble = (half == 0) ? ((byte >> 4) & 0x0F) : (byte & 0x0F);
				const int step = (nibble >= 8) ? (nibble - 16) : nibble;

				double delta = (double) (((1 + std::abs (step << 1)) * stepSize) >> 1);
				if (step <= 0)
				{
					delta = -delta;
				}

				accumulator = std::clamp (accumulator + delta, -32768.0, 32767.0);
				waveform.push_back (accumulator);

				stepSize = (kAdpcmStepTable[8 + step] * stepSize) >> 6;
				stepSize = std::clamp (stepSize, 1, 2000);
			}
		}

		const double cycle = estimateCycleLength (waveform, 0, 0);
		cycleCache[key] = cycle;
		return cycle;
	}

	void keyOnPcm (int channel, std::uint32_t sampleTime)
	{
		PcmVoice& voice = pcm[channel];
		voice.keyed = true;
		++voice.keyOns;
		voice.latchStart = pcmAddressOf (voice);

		// 0 means "no end": a looping voice, or one whose end is behind its start, is stopped by
		// the rate or the volume going to zero, not by a deadline
		const std::uint32_t duration = durationOf (voice);
		voice.gateEnd = (duration > 0) ? sampleTime + duration : 0;
		voice.cycleLength = cycleFor (voice);

		voice.melodic = false;
		if (! rom.empty() && voice.cycleLength > 0.0)
		{
			int fine = 0;
			voice.melodic =
				quantiseFrequency (unitsPerSecond (voice) / voice.cycleLength, &fine)
				<= kHighestMelodicNote;
		}

	}

	void keyOnAdpcm (int channel, std::uint32_t sampleTime)
	{
		AdpcmVoice& voice = adpcm[channel];
		voice.keyed = true;
		++voice.keyOns;
		voice.latchStart = voice.startAddress;
		voice.latchEnd = voice.endAddress;

		// the core mutes an ADPCM voice when its cursor reaches the end address, and one byte is
		// two nibbles
		const double nibbles = (voice.latchEnd > voice.latchStart)
								   ? 2.0 * (double) (voice.latchEnd - voice.latchStart)
								   : 0.0;
		voice.gateEnd = (nibbles > 0.0) ? sampleTime + (std::uint32_t) std::llround (nibbles /
																					adpcmUnitsPerSecond()
																					* streamRate)
										: sampleTime + 1;

		voice.cycleLength = adpcmCycleFor (voice);
		voice.melodic = false;
		if (! rom.empty() && voice.cycleLength > 0.0)
		{
			int fine = 0;
			voice.melodic = quantiseFrequency (adpcmUnitsPerSecond() / voice.cycleLength, &fine)
							<= kHighestMelodicNote;
		}
	}

	/** The core's audible condition for a PCM voice: `output = (volume * sample) >> 14`, so a voice
		sounds while its rate and its volume are both non-zero. There is no key bit anywhere. */
	bool isPcmSounding (int channel) const
	{
		const PcmVoice& voice = pcm[channel];
		if (! voice.keyed || voice.rate == 0 || voice.volume == 0)
		{
			return false;
		}

		return voice.gateEnd == 0 || now < voice.gateEnd;
	}

	bool isAdpcmSounding (int channel) const
	{
		const AdpcmVoice& voice = adpcm[channel];
		return voice.keyed && voice.volume != 0 && now < voice.gateEnd;
	}

	int volumeOf (int volume) const
	{
		// full scale is 0x4000: `(volume * sample) >> 14`
		return std::clamp ((int) std::lround ((double) volume * 15.0 / 16384.0), 0, 15);
	}
};

QSoundDecoder::QSoundDecoder() : impl (std::make_unique<Impl>())
{
	reset (ChipConfig { kDefaultClock, 44100.0 });
}

QSoundDecoder::~QSoundDecoder() = default;

void QSoundDecoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const std::uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	// the ROM belongs to the song, not to the clock
	std::vector<std::uint8_t> rom = impl != nullptr ? std::move (impl->rom) : std::vector<std::uint8_t>();
	const std::uint32_t romMask = impl != nullptr ? impl->romMask : 0u;

	impl = std::make_unique<Impl>();

	// a header that still carries the old 4 MHz clock means the same chip: normalise it the way
	// libvgm does, so the decoder works with either form
	std::uint32_t normalised = (clock > 100000) ? clock : kDefaultClock;
	if (normalised < kLegacyClockCeiling)
	{
		normalised *= kLegacyClockScale;
	}

	impl->clock = (double) normalised;
	impl->streamRate = (sampleRate > 0.0) ? sampleRate : 44100.0;
	impl->rom = std::move (rom);
	impl->romMask = romMask;

	const double adpcmRate = impl->adpcmUnitsPerSecond();
	for (std::size_t i = 0; i < kPcmVoices; ++i)
	{
		impl->tables.push_back (std::make_unique<QSoundNoteTable> (impl->clock, 0.0,
																  &impl->pcm[i].cycleLength,
																  &impl->pcm[i].melodic,
																  &impl->rom));
	}

	for (std::size_t i = 0; i < kAdpcmVoices; ++i)
	{
		impl->tables.push_back (std::make_unique<QSoundNoteTable> (impl->clock, adpcmRate,
																  &impl->adpcm[i].cycleLength,
																  &impl->adpcm[i].melodic,
																  &impl->rom));
	}

	(void) adpcmRate;
}

void QSoundDecoder::setRom (std::vector<std::uint8_t> rom)
{
	impl->romMask = pow2Mask (rom.size());
	impl->rom = std::move (rom);
	impl->cycleCache.clear();
}

void QSoundDecoder::write (const RegisterWrite& w)
{
	// the command's two bytes arrive high-then-low, so `value` holds the high half (`data`)
	const std::uint16_t value = (std::uint16_t) (((std::uint16_t) w.value << 8) | w.value2);
	const std::uint8_t offset = w.reg;

	if (offset < 0x80)
	{
		const int channel = (int) (offset >> 3);
		Impl::PcmVoice& voice = impl->pcm[channel];

		switch (offset & 0x07)
		{
		// One voice's eight registers, by offset - QSound's map is a byte per field, not a bitfield:
		//
		//   0x00  sample bank, and it belongs to the *next* voice (init_register_map in the core)
		//   0x01  start address in that bank
		//   0x02  rate: 0 is silence, and the value is the playback rate this decoder turns into a note
		//   0x03  phase, the fractional part of the rate
		//   0x04  loop length, signed - negative means no loop
		//   0x05  end address
		//   0x06  volume
		//   0x07  echo send
		case 0x00:
			// the bank register belongs to the NEXT voice (init_register_map in the core)
			impl->pcm[(channel + 1) % (int) kPcmVoices].bank = value;
			break;
		case 0x01:
			voice.addr = value;
			// a start while the voice is already sounding is a retrigger: the sample restarts
			if (impl->isPcmSounding (channel))
			{
				impl->keyOnPcm (channel, w.sampleTime);
			}
			break;
		case 0x02:
			voice.rate = value;
			// A rate written while the voice is already sounding is a slide or a vibrato, not a
			// new note: the QSound Logo's voices rewrite the rate nine times inside one sustained
			// note, and keying on every write turned one note into nine. Only the rising edge out
			// of silence starts a note.
			if (value == 0)
			{
				voice.keyed = false;
			}
			else if (voice.volume != 0 && ! impl->isPcmSounding (channel))
			{
				impl->keyOnPcm (channel, w.sampleTime);
			}
			break;
		case 0x03:
			voice.phase = value;
			break;
		case 0x04:
			voice.loopLength = (std::int16_t) value;
			break;
		case 0x05:
			voice.end = (std::int16_t) value;
			break;
		case 0x06:
			voice.volume = (std::int16_t) value;
			if (value == 0)
			{
				voice.keyed = false;
			}
			else if (voice.rate != 0 && ! impl->isPcmSounding (channel))
			{
				impl->keyOnPcm (channel, w.sampleTime);
			}
			break;
		case 0x07:
			voice.echo = (std::int16_t) value;
			break;
		default:
			break;
		}
		return;
	}

	if (offset >= 0x80 && offset < 0x90)
	{
		impl->pcm[offset - 0x80].pan = value;
		return;
	}

	if (offset >= 0x90 && offset < 0x93)
	{
		impl->adpcm[offset - 0x90].pan = value;
		return;
	}

	if (offset >= 0xCA && offset <= 0xD5)
	{
		const int channel = (int) ((offset - 0xCA) / 4);
		Impl::AdpcmVoice& voice = impl->adpcm[channel];

		switch ((offset - 0xCA) % 4)
		{
		case 0:
			voice.startAddress = value;
			break;
		case 1:
			voice.endAddress = value;
			break;
		case 2:
			voice.bank = value;
			break;
		case 3:
			voice.volume = (std::int16_t) value;
			break;
		default:
			break;
		}
		return;
	}

	if (offset >= 0xD6 && offset <= 0xD8)
	{
		const int channel = (int) (offset - 0xD6);
		Impl::AdpcmVoice& voice = impl->adpcm[channel];
		voice.flag = value;

		// a non-zero flag starts the voice (the core clears it and reloads the cursor)
		if (value != 0 && voice.volume != 0)
		{
			impl->keyOnAdpcm (channel, w.sampleTime);
		}
		return;
	}

	// 0x93 / 0xD9.. the echo and the Q1 filter: no notes live there
}

void QSoundDecoder::advance (std::uint32_t sampleTime)
{
	impl->now = sampleTime;

	for (auto& voice : impl->pcm)
	{
		if (voice.keyed && ((voice.rate == 0 || voice.volume == 0)
							|| (voice.gateEnd != 0 && sampleTime >= voice.gateEnd)))
		{
			voice.keyed = false;
		}
	}

	for (auto& voice : impl->adpcm)
	{
		if (voice.keyed && sampleTime >= voice.gateEnd)
		{
			voice.keyed = false;
		}
	}
}

ChannelState QSoundDecoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= channelCount_)
	{
		return state;
	}

	state.present = true;

	if (channel < (int) kPcmVoices)
	{
		const Impl::PcmVoice& voice = impl->pcm[channel];
		state.gate = impl->isPcmSounding (channel);
		state.period = voice.rate;
		state.volume = impl->volumeOf (voice.volume);
		state.keyOns = voice.keyOns;

		const SampledPitch pitch = describeSamplePitch (impl->unitsPerSecond (voice),
														impl->clock / kOutputDivisor, voice.cycleLength,
														voice.melodic);
		state.hz = pitch.hz;
		state.transposeCents = pitch.transposeCents;
		state.rootCents = pitch.rootCents;
		state.pitchSource = pitch.source;
	}
	else
	{
		const Impl::AdpcmVoice& voice = impl->adpcm[channel - (int) kPcmVoices];
		state.gate = impl->isAdpcmSounding (channel - (int) kPcmVoices);
		state.period = 1;		// its clock is fixed, the note table knows the rate
		state.volume = impl->volumeOf (voice.volume);
		state.keyOns = voice.keyOns;

		// the ADPCM voices run at a fixed 8 kHz, so their transposition is against that and their
		// root comes from the sample's own cycle like every other sampled voice
		const SampledPitch pitch = describeSamplePitch (impl->adpcmUnitsPerSecond (),
														impl->adpcmUnitsPerSecond (), voice.cycleLength,
														voice.melodic);
		state.hz = pitch.hz;
		state.transposeCents = pitch.transposeCents;
		state.rootCents = pitch.rootCents;
		state.pitchSource = pitch.source;
	}

	return state;
}

const NoteTable& QSoundDecoder::noteTable (int channel) const
{
	const std::size_t index = (channel >= 0 && channel < channelCount_) ? (std::size_t) channel : 0;
	return *impl->tables[index];
}

Family QSoundDecoder::family (int channel) const
{
	return isDrum (channel) ? Family::adpcmDrum : Family::qsoundPcm;
}

bool QSoundDecoder::isDrum (int channel) const
{
	if (channel < 0 || channel >= channelCount_ || impl->rom.empty())
	{
		return false;
	}

	if (channel < (int) kPcmVoices)
	{
		return ! impl->pcm[channel].melodic;
	}

	return ! impl->adpcm[channel - (int) kPcmVoices].melodic;
}

} // namespace toybox::chips

