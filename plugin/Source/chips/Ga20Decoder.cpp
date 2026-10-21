#include "Ga20Decoder.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	// Above this a "period" is a measurement artifact, not a note - the same C8 ceiling the other
	// sampled players use, because a drum's bytes repeat every few samples.
	constexpr int kHighestMelodicNote = 108;

	// The window a period is measured in. 512 bytes keeps a slide local, which is what makes the
	// period findable at all; the scan walks the sample in half-window steps and gives up after
	// eight kilobytes, because a driver that has not keyed off by then is not playing one note.
	constexpr std::uint32_t kMeasurementUnits = 512;
	constexpr std::uint32_t kScanStep = kMeasurementUnits / 2;
	constexpr std::uint32_t kScanBytes = 8 * 1024;
	constexpr std::uint32_t kMinimumWindow = 128;

	// A period is a lag where the waveform's correlation dips below this: a steady tone reaches
	// about -1 at its half period, while noise stays near 0 at every lag.
	constexpr double kPeriodTrough = -0.5;

	// ...and the period is the peak that follows it.
	constexpr double kPeriodPeak = 0.5;

	// A sample ends on a 0x00 byte; one that never reaches one is treated as open-ended rather than
	// scanned through a whole ROM.
	constexpr std::uint32_t kMaxSampleScan = 256 * 1024;

	/** The mask the chip's address is folded with: the ROM is covered as a power-of-two buffer
		(`pow2_mask` in the core), so a sample that runs off the end wraps instead of reading
		elsewhere. */
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

	/** One table per voice: the rate register is a divider and the sample each voice latched has
		its own cycle, so the note is `byteClock / (256 - rate) / cycle`. A sample that does not
		repeat, or repeats at a rate no note is written at, is the C4 trigger. */
	class Ga20NoteTable final : public NoteTable
	{
	public:
		Ga20NoteTable (double clock, const double* sampleCycle, const bool* sampleMelodic)
			: byteClock (clock > 0.0 ? clock : 44100.0)
			, cycleLength (sampleCycle)
			, melodic (sampleMelodic) {}

		int noteFor (std::uint32_t rate, int, int* finePitch) const override
		{
			if (rate >= 0x100 || melodic == nullptr || ! *melodic || cycleLength == nullptr
				|| *cycleLength <= 0.0)
			{
				*finePitch = 0;
				return 60;		// a drum: the C4 trigger
			}

			const double bytesPerSecond = byteClock / (double) (0x100 - rate);
			return quantiseFrequency (bytesPerSecond / *cycleLength, finePitch);
		}

	private:
		double byteClock;
		const double* cycleLength;
		const bool* melodic;
	};
}

struct Ga20Decoder::Impl
{
	struct Voice
	{
		std::uint16_t start = 0;		// in 16-byte units, exactly as the two registers hold it
		std::uint8_t rate = 0;
		std::uint8_t volume = 0;
		bool keyed = false;

		std::uint32_t latchStart = 0;	// byte address at the key-on
		std::uint32_t gateEnd = 0;
		double cycleLength = 0.0;
		bool melodic = false;
		std::uint32_t keyOns = 0;
	};

	Voice voices[4];
	std::vector<std::uint8_t> rom;
	std::uint32_t romMask = 0;
	double byteClock = 44100.0;		// the chip's own rate: clock / 4
	double outRate = 44100.0;		// the rate the note times and gates are in
	std::map<std::uint32_t, std::uint32_t> lengthCache;
	std::map<std::uint32_t, double> cycleCache;
	std::unique_ptr<Ga20NoteTable> tables[4];
	std::uint32_t now = 0;

	std::uint8_t byteAt (std::uint32_t address) const
	{
		const std::uint32_t index = address & romMask;
		return index < rom.size() ? rom[index] : 0;
	}

	/** The byte rate the core plays this voice at: one byte every `256 - rate` samples of the
		chip's own clock, which runs at `clock / 4` (iremga20.c hands `cfg->clock / 4` to
		INIT_DEVINF, and libvgm resamples that down to the output rate afterwards). Using the
		output rate here made every pitch about twenty times too low, and since a note below 0 is
		not a note, every voice in every song became the C4 trigger. */
	double bytesPerSecond (const Voice& voice) const
	{
		return byteClock / (double) (0x100 - voice.rate);
	}

	/** How many bytes the sample holds before its 0x00 terminator, measured once per address. */
	std::uint32_t lengthOf (std::uint32_t start)
	{
		if (rom.empty())
		{
			return 0;
		}

		const auto cached = lengthCache.find (start);
		if (cached != lengthCache.end())
		{
			return cached->second;
		}

		std::uint32_t length = 0;
		while (length < kMaxSampleScan && byteAt (start + length) != 0)
		{
			++length;
		}

		if (length >= kMaxSampleScan)
		{
			length = 0;		// no terminator in reach: do not invent a length
		}

		lengthCache[start] = length;
		return length;
	}

	/** The best period in a window: the shortest lag whose correlation dips below `kPeriodTrough`,
		or 0 for a window with no such dip. Correlation at the half-period of a steady tone is
		about -1, a drum's noise stays near 0 at every lag. */
	static double bestPeriod (const std::vector<double>& waveform)
	{
		const std::size_t size = waveform.size();
		if (size < 64)
		{
			return 0.0;
		}

		double mean = 0.0;
		for (double sample : waveform)
		{
			mean += sample;
		}

		mean /= (double) size;

		// The period is the *peak* after the first trough, not the trough itself: a symmetric
		// waveform anti-correlates at half its period (a square dips to -1 at lag 8 when the period
		// is 16), and returning that would report every tone an octave high. Of the peaks that
		// follow, the smallest one within a tenth of the best is the period - a window edge can put
		// a lag one sample off, and its peak is weaker than the true one.
		const std::size_t maxLag = std::min<std::size_t> (size / 2, 512);
		std::vector<double> correlation (maxLag + 1, 0.0);
		bool sawTrough = false;
		std::size_t troughLag = 0;
		double best = 0.0;

		for (std::size_t lag = 2; lag <= maxLag; ++lag)
		{
			double product = 0.0;
			double energyA = 0.0;
			double energyB = 0.0;
			for (std::size_t i = 0; i + lag < size; ++i)
			{
				const double a = waveform[i] - mean;
				const double b = waveform[i + lag] - mean;
				product += a * b;
				energyA += a * a;
				energyB += b * b;
			}

			if (energyA <= 0.0 || energyB <= 0.0)
			{
				continue;
			}

			correlation[lag] = product / std::sqrt (energyA * energyB);
			if (! sawTrough)
			{
				sawTrough = correlation[lag] <= kPeriodTrough;
				troughLag = lag;
			}
			else if (correlation[lag] > best)
			{
				best = correlation[lag];
			}
		}

		if (! sawTrough || best < kPeriodPeak)
		{
			return 0.0;
		}

		// from the trough, not from the start: a smooth waveform correlates with itself at lag 2 or
		// 3 by mere continuity, and returning that reported every voice as a two-byte cycle
		for (std::size_t lag = troughLag; lag <= maxLag; ++lag)
		{
			if (correlation[lag] >= best * 0.9 && correlation[lag] >= kPeriodPeak)
			{
				return (double) lag;
			}
		}

		return 0.0;
	}

	/** The sample's own cycle in bytes, measured once per start address.

		Two differences from `estimateCycleLength()`, both forced by this chip. Its samples are
		*slides*: the bass at 0x63320 of the stage theme runs from a 175-byte cycle down to a
		251-byte one, so one window over the whole sample averages the period away and finds
		nothing. The scan therefore walks 512-byte windows from the start and keeps the first one
		that has a period - the note the driver keyed, before the slide moved it. A window is 512
		bytes so that a lag is local enough for a slide to still be periodic; that also caps the
		measurable cycle at 256 bytes, which these byte rates never reach (a 163-byte cycle is
		105 Hz). Anything the scan cannot measure - noise, a drum sweep - is 0, the C4 trigger.

		The trough has to come before the peak because that is what tells a period from a
		smoothness: a tone anti-correlates at half its cycle, while a drum's noise never dips. */
	double cycleFor (std::uint32_t start, std::uint32_t length)
	{
		if (rom.empty() || length == 0)
		{
			return 0.0;
		}

		const auto cached = cycleCache.find (start);
		if (cached != cycleCache.end())
		{
			return cached->second;
		}

		double cycle = 0.0;
		const std::uint32_t limit = std::min<std::uint32_t> (length, kScanBytes);
		for (std::uint32_t offset = 0; cycle <= 0.0 && offset + kMinimumWindow <= limit;
			 offset += kScanStep)
		{
			const std::uint32_t window =
				std::min<std::uint32_t> (limit - offset, kMeasurementUnits);
			std::vector<double> waveform (window);
			for (std::uint32_t i = 0; i < window; ++i)
			{
				waveform[i] = (double) ((int) byteAt (start + offset + i) - 0x80);
			}

			cycle = bestPeriod (waveform);
		}

		cycleCache[start] = cycle;
		return cycle;
	}

	void keyOn (Voice& voice, std::uint32_t time)
	{
		voice.latchStart = (std::uint32_t) voice.start << 4;
		++voice.keyOns;

		// the gate is a count of the chip's own samples, and the note times are output samples
		const std::uint32_t length = lengthOf (voice.latchStart);
		const std::uint32_t nativePerByte = 0x100 - voice.rate;
		voice.gateEnd = (length > 0)
							? time + (std::uint32_t) std::llround ((double) length
									* (double) nativePerByte * outRate / byteClock)
							: 0;

		voice.cycleLength = cycleFor (voice.latchStart, length);
		voice.melodic = false;
		if (voice.cycleLength > 0.0 && voice.rate < 0x100)
		{
			int fine = 0;
			const int note = quantiseFrequency (bytesPerSecond (voice) / voice.cycleLength, &fine);
			voice.melodic = note >= 0 && note <= kHighestMelodicNote;
		}

		voice.keyed = true;
	}

	/** The core's control bit: writing 2 plays, anything else stops. A sample whose first byte is
		the terminator never sounds (`irem_ga20_cache_samples` clears `play`), which is mirrored
		here so a key-on into silence does not become a note. */
	void writeControl (Voice& voice, std::uint8_t value, std::uint32_t time)
	{
		if ((value & 0x02) == 0)
		{
			voice.keyed = false;
			return;
		}

		const std::uint32_t start = (std::uint32_t) voice.start << 4;
		if (! rom.empty() && byteAt (start) == 0)
		{
			voice.keyed = false;
			return;
		}

		keyOn (voice, time);
	}

	int volumeOf (const Voice& voice) const
	{
		// `data * 256 / (data + 10)`, scaled to the 0..127 the extractor carries
		const int level = (int) ((std::uint32_t) voice.volume * 256u / ((std::uint32_t) voice.volume + 10u));
		return std::min (127, (level * 127 + 128) / 256);
	}

	bool sounding (const Voice& voice) const
	{
		if (! voice.keyed || voice.volume == 0)
		{
			return false;
		}

		return voice.gateEnd == 0 || now < voice.gateEnd;
	}
};

Ga20Decoder::Ga20Decoder()
	: impl (std::make_unique<Impl>())
{
}

Ga20Decoder::~Ga20Decoder() = default;

void Ga20Decoder::reset (std::uint32_t clock, double sampleRate)
{
	// the ROM belongs to the song, not to the clock
	std::vector<std::uint8_t> rom = std::move (impl->rom);
	const std::uint32_t mask = impl->romMask;

	*impl = Impl();
	impl->rom = std::move (rom);
	impl->romMask = mask;
	impl->outRate = sampleRate > 0.0 ? sampleRate : 44100.0;

	// The core's own sample rate is the chip clock divided by four (iremga20.c: INIT_DEVINF gets
	// `cfg->clock / 4`), and libvgm resamples that down to the output rate. `256 - rate` counts
	// *those* samples, so the byte rate has the clock in it - unlike the GA20 of an old VGM with
	// no clock field, where libvgm starts no device at all and the fallback only matters to the
	// decoder.
	impl->byteClock = (clock > 0) ? (double) clock / 4.0 : impl->outRate;

	for (int i = 0; i < (int) Ga20Decoder::numChannels; ++i)
	{
		impl->tables[i] = std::make_unique<Ga20NoteTable> (impl->byteClock,
														   &impl->voices[i].cycleLength,
														   &impl->voices[i].melodic);
	}
}

void Ga20Decoder::setRom (std::vector<std::uint8_t> rom)
{
	impl->romMask = pow2Mask (rom.size());
	impl->rom = std::move (rom);
	impl->lengthCache.clear();
	impl->cycleCache.clear();
}

void Ga20Decoder::write (const RegisterWrite& write)
{
	const std::uint8_t offset = (std::uint8_t) (write.reg & 0x1F);
	const int channel = offset >> 3;
	if (channel < 0 || channel >= (int) Ga20Decoder::numChannels)
	{
		return;
	}

	Impl::Voice& voice = impl->voices[channel];
	switch (offset & 0x07)
	{
	case 0:		// start, low: bits 4..11 of the byte address
		voice.start = (std::uint16_t) ((voice.start & 0xFF00) | (write.value & 0xFF));
		break;

	case 1:		// start, high: bits 12..19
		voice.start = (std::uint16_t) ((voice.start & 0x00FF) | ((std::uint16_t) write.value << 8));
		break;

	case 2:		// end, low - written by every driver, never read by the core
	case 3:
		break;

	case 4:
		voice.rate = write.value;
		break;

	case 5:
		voice.volume = write.value;
		break;

	case 6:
		impl->writeControl (voice, write.value, write.sampleTime);
		break;

	default:
		break;
	}
}

void Ga20Decoder::advance (std::uint32_t sampleTime)
{
	impl->now = sampleTime;
}

ChannelState Ga20Decoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= (int) Ga20Decoder::numChannels)
	{
		return state;
	}

	const Impl::Voice& voice = impl->voices[channel];
	state.present = true;
	state.gate = impl->sounding (voice);
	state.keyOns = voice.keyOns;
	state.period = voice.rate;
	state.volume = impl->volumeOf (voice);

	return state;
}

const NoteTable& Ga20Decoder::noteTable (int channel) const
{
	const std::size_t index =
		(channel >= 0 && channel < (int) Ga20Decoder::numChannels) ? (std::size_t) channel : 0;
	return *impl->tables[index];
}

Family Ga20Decoder::family (int channel) const
{
	return isDrum (channel) ? Family::adpcmDrum : Family::ga20Pcm;
}

bool Ga20Decoder::isDrum (int channel) const
{
	if (channel < 0 || channel >= (int) Ga20Decoder::numChannels)
	{
		return true;
	}

	return ! impl->voices[channel].melodic;
}

} // namespace toybox::chips
