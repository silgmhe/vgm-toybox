#include "Rf5c68Decoder.h"

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
	// The clock of an RF5C164/68 when the VGM header does not say (Sonic CD's rips leave field 0x98
	// at 0xFFFFFFFF). 12.5 MHz is what every board and libvgm's own default use.
	constexpr std::uint32_t kDefaultClock = 12500000;

	// The core's sample rate is `clock / 384` (rf5c68.c: INIT_DEVINF), and the address advances by
	// `step` in 1/2048-byte units per sample, so step 2048 is one byte per sample.
	constexpr double kClockDivisor = 384.0;
	constexpr std::uint32_t kOneBytePerSample = 2048;

	// Above this a "period" is a measurement artifact, not a note - the same C8 ceiling the other
	// sampled players use.
	constexpr int kHighestMelodicNote = 108;

	constexpr std::uint32_t kRamBytes = 64 * 1024;

	// A sample ends at a 0xFF byte; one that never reaches one is treated as open-ended rather than
	// scanned through the whole RAM.
	constexpr std::uint32_t kMaxSampleScan = kRamBytes;

	// The marker a channel gets when nothing says how long its sample is: 80 ms at 44.1 kHz, the
	// convention the OPN's ADPCM-A uses.
	constexpr std::uint32_t kGateSamples = 3528;

	// The window a cycle is measured in, and how far the scan walks. These are the GA20's numbers for
	// the same reason: a Sega CD instrument opens with an attack that is not yet periodic (the four
	// samples of Palmtree Panic's first bank only measure from 768-2560 bytes in), so the scan walks
	// 512-byte windows from the start and keeps the first one with a period.
	constexpr std::uint32_t kMeasurementUnits = 512;
	constexpr std::uint32_t kScanStep = kMeasurementUnits / 2;
	constexpr std::uint32_t kScanBytes = 8 * 1024;
	constexpr std::uint32_t kMinimumWindow = 128;

	// A period is a lag whose correlation dips below the first value and peaks above the second: a
	// steady tone anti-correlates at half its cycle, noise never dips.
	constexpr double kPeriodTrough = -0.5;
	constexpr double kPeriodPeak = 0.5;
}

struct Rf5c68Decoder::Impl
{
	struct Channel
	{
		std::uint8_t envelope = 0;
		std::uint8_t pan = 0;
		std::uint16_t step = 0;		// 0x02 / 0x03
		std::uint16_t loopStart = 0;	// 0x04 / 0x05, in 256-byte units
		std::uint8_t start = 0;		// 0x06, in 256-byte units
		bool enabled = false;
		bool keyed = false;

		std::uint32_t latchStart = 0;	// byte address at the key-on
		double cycleLength = 0.0;
		bool melodic = false;
		std::uint32_t gateEnd = 0;		// 0 when the sample loops
		std::uint32_t keyOns = 0;
	};

	Channel channels[numChannels];
	std::vector<std::uint8_t> ram;
	std::map<std::uint32_t, double> cycleCache;

	double clockRate = (double) kDefaultClock;
	double outRate = 44100.0;
	std::uint8_t bank = 0;			// 0x07 bit 6: which channel 0x00-0x06 address
	bool master = true;				// 0x07 bit 7
	std::uint32_t now = 0;

	std::uint8_t byteAt (std::uint32_t address) const
	{
		if (ram.empty())
		{
			return 0xFF;		// no RAM reads as "end of sample", which is what the chip hears
		}

		return ram[address & 0xFFFF];
	}

	/** The chip's own sample rate: `clock / 384`, and the rate one byte per sample would play at. */
	double chipRate (void) const
	{
		return clockRate / kClockDivisor;
	}

	double bytesPerSecond (const Channel& channel) const
	{
		return (double) channel.step * chipRate() / (double) kOneBytePerSample;
	}

	/** Bytes from `start` to the terminating 0xFF, which is the only length the chip has. */
	std::uint32_t lengthOf (std::uint32_t start) const
	{
		std::uint32_t length = 0;
		while (length < kMaxSampleScan && byteAt (start + length) != 0xFF)
		{
			++length;
		}

		return (length >= kMaxSampleScan) ? 0 : length;
	}

	/** The best period in a window: the shortest lag whose correlation dips below `kPeriodTrough`
		and then peaks above `kPeriodPeak`, or 0 for a window with no such dip. */
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

		// from the trough, not from the start: a smooth waveform correlates with itself at lag 2 or 3
		// by mere continuity, and returning that reports every tone an octave or two high
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

		`estimateCycleLength()` is the shared measurement, but it wants a steady waveform from the
		first byte and demands a strong peak (0.80) - and a Sega CD instrument's attack is neither
		(this bank's four samples measure from 768 to 2560 bytes in). So the scan walks 512-byte
		windows and keeps the first with a period, the same rule the GA20 decoder uses for its slides.
		Anything it cannot measure - a drum, a chord, noise - is 0, the C4 trigger. */
	double cycleFor (std::uint32_t start)
	{
		const auto cached = cycleCache.find (start);
		if (cached != cycleCache.end())
		{
			return cached->second;
		}

		double cycle = 0.0;
		const std::uint32_t length = lengthOf (start);
		const std::uint32_t limit = std::min<std::uint32_t> (length, kScanBytes);
		for (std::uint32_t offset = 0; cycle <= 0.0 && offset + kMinimumWindow <= limit;
			 offset += kScanStep)
		{
			const std::uint32_t window = std::min<std::uint32_t> (limit - offset, kMeasurementUnits);
			std::vector<double> waveform (window);
			for (std::uint32_t i = 0; i < window; ++i)
			{
				waveform[i] = (double) byteAt (start + offset + i);
			}

			cycle = bestPeriod (waveform);
		}

		cycleCache[start] = cycle;
		return cycle;
	}

	void keyOff (Channel& channel)
	{
		channel.keyed = false;
		channel.gateEnd = 0;
	}

	void keyOn (Channel& channel, std::uint32_t time)
	{
		++channel.keyOns;
		channel.keyed = true;
		channel.latchStart = (std::uint32_t) channel.start << 8;
		channel.cycleLength = cycleFor (channel.latchStart);
		channel.gateEnd = 0;

		const double rate = bytesPerSecond (channel);
		channel.melodic = false;
		if (channel.cycleLength > 0.0 && rate > 0.0)
		{
			int fine = 0;
			const int note = quantiseFrequency (rate / channel.cycleLength, &fine);
			channel.melodic = note >= 0 && note <= kHighestMelodicNote;
		}

		// A sample ends at a 0xFF byte, and the core then jumps to the loop address. If that byte is
		// also 0xFF the voice dies, so the length is exact; otherwise it loops until the driver
		// clears the channel's bit in 0x08.


		const std::uint32_t loopAddress = (std::uint32_t) channel.loopStart << 8;
		const bool diesAtTheEnd = byteAt (loopAddress) == 0xFF;
		const std::uint32_t length = lengthOf (channel.latchStart);
		if (length == 0)
		{
			// no RAM (or no sample at that address): nothing tells the note's length, so it gets the
			// same 80 ms marker the OPN's ADPCM-A uses rather than holding until the driver clears
			// the channel - a driver that leaves a channel enabled is the normal case
			channel.gateEnd = time + kGateSamples;
		}
		else if (diesAtTheEnd && rate > 0.0)
		{
			channel.gateEnd = time + (std::uint32_t) std::llround ((double) length * outRate / rate);
		}
	}

	int velocityOf (const Channel& channel) const
	{
		// the envelope is a level, not an attenuation: 0..255 straight to 0..127
		return std::min (127, ((int) channel.envelope + 1) / 2);
	}

	bool sounding (const Channel& channel) const
	{
		if (! master || ! channel.enabled || ! channel.keyed || channel.envelope == 0)
		{
			return false;
		}

		return channel.gateEnd == 0 || now < channel.gateEnd;
	}

	/** One table per channel: the note is the step against the chip's own rate, and both live on the
		channel's state, so the table reads through rather than caching a number a write would stale. */
	class Note final : public NoteTable
	{
	public:
		Note (const Impl& owner, int index)
			: impl (owner)
			, channel (index) {}

		int noteFor (std::uint32_t step, int, int* finePitch) const override
		{
			*finePitch = 0;

			const Channel& voice = impl.channels[channel];
			if (step == 0 || ! voice.melodic || voice.cycleLength <= 0.0)
			{
				return 60;		// a drum (or silence): the C4 trigger
			}

			// L1: the step against one byte per sample, the register's own unity
			return quantisePlaybackRate (impl.bytesPerSecond (voice), impl.chipRate(), finePitch);
		}

	private:
		const Impl& impl;
		int channel;
	};

	std::unique_ptr<Note> tables[numChannels];
};

Rf5c68Decoder::Rf5c68Decoder() : impl (std::make_unique<Impl>())
{
	reset (ChipConfig { kDefaultClock, 44100.0 });
}

Rf5c68Decoder::~Rf5c68Decoder() = default;

void Rf5c68Decoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const std::uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	// the RAM belongs to the song, not to the run
	std::vector<std::uint8_t> ram = std::move (impl->ram);

	impl = std::make_unique<Impl>();
	impl->ram = std::move (ram);
	impl->clockRate = (clock > 0) ? (double) clock : (double) kDefaultClock;
	impl->outRate = (sampleRate > 0.0) ? sampleRate : 44100.0;

	for (int i = 0; i < channelCount_; ++i)
	{
		impl->tables[i] = std::make_unique<Impl::Note> (*impl, i);
	}
}

void Rf5c68Decoder::setRam (std::vector<std::uint8_t> ram)
{
	impl->ram = std::move (ram);
	impl->cycleCache.clear();
}

void Rf5c68Decoder::write (const RegisterWrite& w)
{
	const std::uint8_t offset = w.reg;

	if (offset == 0x08)
	{
		// a cleared bit enables the channel, and enabling one is the only key-on this chip has
		for (int channel = 0; channel < channelCount_; ++channel)
		{
			Impl::Channel& voice = impl->channels[channel];
			const bool on = ((~w.value >> channel) & 1) != 0;
			if (on && ! voice.enabled)
			{
				voice.enabled = true;
				impl->keyOn (voice, w.sampleTime);
			}
			else if (! on && voice.enabled)
			{
				voice.enabled = false;
				impl->keyOff (voice);
			}
		}

		return;
	}

	if (offset == 0x07)
	{
		// bit 6 selects the channel the low registers address, bit 7 is the output switch
		impl->master = (w.value & 0x80) != 0;
		if ((w.value & 0x40) != 0)
		{
			impl->bank = (std::uint8_t) (w.value & 0x07);
		}

		if (! impl->master)
		{
			for (auto& voice : impl->channels)
			{
				impl->keyOff (voice);
			}
		}

		return;
	}

	Impl::Channel& voice = impl->channels[impl->bank & 0x07];
	switch (offset)
	{
	// One voice's register block, by offset (the same layout the RF5C164 inherits):
	//
	//   0x00  envelope: 0 stops the voice, 0x7F is full scale
	//   0x01  pan, one nibble per side
	//   0x02/0x03  step, low byte then high - the playback rate, and the chip's only pitch
	//   0x04/0x05  loop start address, low then high, in 256-byte units
	//   0x06  start address, in 256-byte units; writing it starts the voice
	case 0x00: voice.envelope = w.value; break;
	case 0x01: voice.pan = w.value; break;
	case 0x02: voice.step = (std::uint16_t) ((voice.step & 0xFF00) | w.value); break;
	case 0x03: voice.step = (std::uint16_t) ((voice.step & 0x00FF) | ((std::uint16_t) w.value << 8)); break;
	case 0x04: voice.loopStart = (std::uint16_t) ((voice.loopStart & 0xFF00) | w.value); break;
	case 0x05: voice.loopStart = (std::uint16_t) ((voice.loopStart & 0x00FF) | ((std::uint16_t) w.value << 8)); break;
	case 0x06: voice.start = w.value; break;
	default: break;
	}
}

void Rf5c68Decoder::advance (std::uint32_t sampleTime)
{
	impl->now = sampleTime;
}

ChannelState Rf5c68Decoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= channelCount_)
	{
		return state;
	}

	const Impl::Channel& voice = impl->channels[channel];
	state.present = true;
	state.gate = impl->sounding (voice);
	state.keyOns = voice.keyOns;
	state.period = voice.step;
	state.volume = impl->velocityOf (voice);

	const SampledPitch pitch = describeSamplePitch (impl->bytesPerSecond (voice), impl->chipRate(),
													voice.cycleLength, voice.melodic);
	state.hz = pitch.hz;
	state.transposeCents = pitch.transposeCents;
	state.rootCents = pitch.rootCents;
	state.pitchSource = pitch.source;

	return state;
}

const NoteTable& Rf5c68Decoder::noteTable (int channel) const
{
	const int index = (channel >= 0 && channel < channelCount_) ? channel : 0;
	return *impl->tables[index];
}

Family Rf5c68Decoder::family (int channel) const
{
	return isDrum (channel) ? Family::adpcmDrum : Family::rf5cPcm;
}

bool Rf5c68Decoder::isDrum (int channel) const
{
	if (channel < 0 || channel >= channelCount_)
	{
		return true;
	}

	return ! impl->channels[channel].melodic;
}

} // namespace toybox::chips
