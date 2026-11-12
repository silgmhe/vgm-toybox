#include "GbcApuDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	// The frame sequencer runs at 512 Hz off the CPU clock: 8192 cycles per step at 4.19 MHz.
	constexpr double kFrameCycles = 8192.0;
	constexpr uint32_t kDefaultClock = 4194304;

	/** The frequency register counts *down* from this: the divider reloads with `2048 - value`,
		which is why a larger register value is a higher note and the tables below read
		`kFrequencyRegisterModulus - period`. */
	constexpr double kFrequencyRegisterModulus = 2048.0;

	/** **The wave channel's thirty-two steps live in RAM**, not in a register: `0xFF30-0xFF3F`, which
		is offset `0x20-0x2F` from the register block's base in a `0xB3` command. Two 4-bit samples
		share a byte, and - the opposite of the WonderSwan - an **even** step takes the **high**
		nibble (`gb_mame.c:990-997`). */
	constexpr int kWaveBytes = 16;
	constexpr int kWaveSteps = kWaveBytes * 2;
	constexpr uint8_t kWaveRamOffset = 0x20;

	/** A 4-bit sample is centred on 8 and the core makes it bipolar by subtracting 8
		(`gb_mame.c:996`), so a decoded step is `(value - 8) / 8`. */
	constexpr float kWaveCentre = 8.0f;

	/** The counter ticks every **two** CPU cycles and the offset advances once per reload
		(`gb_mame.c:962-1000`), so a step lasts `2 * (2048 - f)` clocks and the tone is that over
		thirty-two steps - the `clock / (64 * (2048 - f))` the note table already reports. */
	constexpr double kWaveCyclesPerStep = 2.0;

	/** A cache with a bound: these files rewrite the table when they change instrument (fifteen to
		forty-four shapes a song), so the decode has to be a hit. Each entry is 128 bytes. */
	constexpr std::size_t kWaveCacheLimit = 64;

	/** FNV-1a over the sixteen bytes. */
	uint64_t hashOfWave (const uint8_t* bytes)
	{
		uint64_t hash = 1469598103934665603ull;

		for (int i = 0; i < kWaveBytes; ++i)
		{
			hash = (hash ^ (uint64_t) bytes[i]) * 1099511628211ull;
		}

		return hash;
	}

	/** Pulse and wave: frequency = clock / (divider * (2048 - register)). The pulse channels
		divide the clock by 32, the wave channel by 64 - the same register is an octave lower
		there, which is why the divider lives in the table and not in the decoder. */
	class GbToneTable final : public NoteTable
	{
	public:
		GbToneTable (double clock, double divider) : cpuClock (clock), divider (divider) {}

		int noteFor (uint32_t period, int, int* finePitch) const override
		{
			if (period > 2047)
			{
				return -1;
			}

			const double divisor = kFrequencyRegisterModulus - (double) period;
			return quantiseFrequency (cpuClock / (divider * divisor), finePitch);
		}

	private:
		double cpuClock;
		double divider;
	};

	/** NR43, the noise register: the clock shift in the high nibble and the dividing ratio in the
		low three bits. A ratio of zero means a divisor of 8 rather than 16 * 0, which is why the
		table starts at half the step of every ratio above it. */
	constexpr uint32_t kNoiseShiftMask = 0x0F;
	constexpr int kNoiseShiftPosition = 4;
	constexpr uint32_t kNoiseRatioMask = 0x07;
	constexpr double kNoiseBaseClock = 262144.0;	// the APU clock halved, which is what noise counts

	/** NRx2, the envelope register: initial volume in the high nibble, direction in bit 3, sweep
		period in the low three bits. Bit 3 is also part of the DAC's gate - the converter is on
		while any of bits 7..3 is set - which is why the enable test reads 0xF8 and not the
		direction bit alone. */
	constexpr uint8_t kEnvelopeVolumeShift = 4;
	constexpr uint8_t kEnvelopeVolumeMask = 0x0F;
	constexpr uint8_t kEnvelopeIncrease = 0x08;
	constexpr uint8_t kEnvelopePeriodMask = 0x07;
	constexpr uint8_t kEnvelopeDacMask = 0xF8;

	/** NR10, the sweep register, which uses the same two bit positions as the envelope for a
		different purpose: bit 3 is the direction of the sweep (a falling pitch when set) and the
		low three are how far it steps. Same values, different meanings - which is why they are
		not shared with the envelope's. */
	constexpr uint8_t kSweepNegate = 0x08;
	constexpr uint8_t kSweepShiftMask = 0x07;

	/** NR43's bit 3 picks the noise width - 15 bits when clear, 7 when set - and sits above the
		ratio the noise table reads. */
	constexpr uint8_t kNoiseWidthMode = 0x08;

	/** The noise channel has no pitch: its shift/divisor pair is mapped onto GM drum keys, the
		way the 2A03's 16 period indices are. Each step of the shift halves the frequency, so the
		buckets follow the logarithm of it and stay monotonic. */
	class GbNoiseTable final : public NoteTable
	{
	public:
		int noteFor (uint32_t period, int, int* finePitch) const override
		{
			if (finePitch != nullptr)
			{
				*finePitch = 0;
			}

			const int shift = (int) ((period >> kNoiseShiftPosition) & kNoiseShiftMask);
			const int code = (int) (period & kNoiseRatioMask);
			const double divisor = (code == 0) ? 8.0 : 16.0 * (double) code;	// 8, 16 ... 112
			const double hz = kNoiseBaseClock / (divisor * std::exp2 ((double) shift));
			const double octaves = std::log2 (32768.0 / std::max (hz, 0.001));
			const long bucket = std::lround (octaves * 10.0 / 19.0);
			const int level = (int) std::clamp<long> (bucket, 0, 10);

			return kDrumBaseKey + level;
		}

	private:
		static constexpr int kDrumBaseKey = 38;		// GM acoustic snare: the 2A03 starts there too
	};
}

struct GbcApuDecoder::Impl
{
	struct Envelope
	{
		int initialVolume = 0;		// NRx2 bits 7-4
		bool increase = false;		// bit 3
		int period = 0;				// bits 2-0, in 64 Hz ticks
		bool dacEnabled = false;	// bits 7-3, any of them: without a DAC the channel is silent
		int volume = 0;				// current output volume, 0..15
		int timer = 0;
	};

	struct Pulse
	{
		Envelope envelope;
		bool lengthEnable = false;
		int lengthLoad = 0;			// NRx1 bits 5-0, the count is 64 minus this
		int length = 0;
		int duty = 0;
		int frequency = 0;			// 11 bits, the value the registers hold
		bool sweepEnabled = false;
		bool sweepNegate = false;
		int sweepPeriod = 0;
		int sweepShift = 0;
		int sweepTimer = 0;
		bool overflow = false;		// the sweep pushed the frequency past 2047
	};

	struct Wave
	{
		bool dacEnabled = false;	// NR30 bit 7
		bool lengthEnable = false;
		int lengthLoad = 0;			// NR31, 8 bits: the count is 256 minus this
		int length = 0;
		int volumeCode = 0;			// NR32 bits 6-5: 0 mute, 1 full, 2 half, 3 quarter
		int frequency = 0;
	};

	struct NoiseChannel
	{
		Envelope envelope;
		bool lengthEnable = false;
		int lengthLoad = 0;
		int length = 0;
		int shift = 0;				// NR43 bits 7-4
		int divisorCode = 0;		// bits 2-0
		bool widthMode = false;		// bit 3, 7-bit LFSR instead of 15-bit
	};

	Pulse pulses[2];
	Wave wave;
	NoiseChannel noiseChannel;
	bool powered = false;		// NR52 starts at 0: a driver has to switch the APU on

	double cpuClock = (double) kDefaultClock;
	double sampleRate = 44100.0;
	double frameStep = kFrameCycles * 44100.0 / (double) kDefaultClock;
	double frameAccumulator = 0.0;
	int frameIndex = 0;
	uint32_t currentSample = 0;

	std::unique_ptr<NoteTable> pulseTable;
	std::unique_ptr<NoteTable> waveTable;
	std::unique_ptr<NoteTable> noiseTable;

	/** **The wave channel's own memory**, sixteen bytes the driver uploads at offsets 0x20-0x2F.

		The panel needs it and the note does not, which is why it arrived later than the pitch: the
		same 16 bytes are the whole shape, and a driver rewrites them whenever it changes instrument.
	*/
	uint8_t waveRam[kWaveBytes] = {};

	/** Decoded tables keyed by their sixteen bytes. `mutable` because `channelState` is const and
		the vector it hands out is immutable once decoded. */
	mutable std::map<uint64_t, std::shared_ptr<const std::vector<float>>> waveCache;

	/** **The thirty-two steps the wave channel walks**, read out of `waveRam`.

		The nibble order is the core's (`gb_mame.c:990-997`) and it is the **reverse** of the
		WonderSwan's: an even step takes the **high** nibble. Getting it backwards mirrors every
		two-step pair - a shape that reads as slightly different rather than as an error, which is
		why a test pins both steps of the first byte.
	*/
	std::shared_ptr<const std::vector<float>> waveFor (void) const
	{
		const uint64_t key = hashOfWave (waveRam);

		const auto found = waveCache.find (key);

		if (found != waveCache.end ())
		{
			return found->second;
		}

		auto wave = std::make_shared<std::vector<float>> ();
		wave->reserve (kWaveSteps);

		for (int step = 0; step < kWaveSteps; ++step)
		{
			const uint8_t byte = waveRam[step / 2];
			const uint8_t nibble = (step % 2 == 0) ? (uint8_t) (byte >> 4) : (uint8_t) (byte & 0x0F);

			wave->push_back (((float) nibble - kWaveCentre) / kWaveCentre);
		}

		if (waveCache.size () >= kWaveCacheLimit)
		{
			// A song's vocabulary is a few dozen shapes; a driver that writes a new one every frame
			// must not grow this for the length of the song.
			waveCache.clear ();
		}

		return waveCache.emplace (key, std::move (wave)).first->second;
	}

	void writeEnvelope (Envelope& envelope, uint8_t value)
	{
		envelope.initialVolume = (value >> kEnvelopeVolumeShift) & kEnvelopeVolumeMask;
		envelope.increase = (value & kEnvelopeIncrease) != 0;
		envelope.period = value & kEnvelopePeriodMask;
		envelope.dacEnabled = (value & kEnvelopeDacMask) != 0;
		if (! envelope.dacEnabled)
		{
			envelope.volume = 0;
		}
	}

	void triggerEnvelope (Envelope& envelope)
	{
		envelope.volume = envelope.initialVolume;
		envelope.timer = envelope.period;
	}

	void triggerPulse (Pulse& pulse, bool hasSweep)
	{
		if (pulse.length == 0)
		{
			pulse.length = 64 - pulse.lengthLoad;
		}

		triggerEnvelope (pulse.envelope);

		if (hasSweep)
		{
			// a period of 0 means 8: the sweep clock is never stopped by it
			pulse.overflow = false;
			pulse.sweepTimer = (pulse.sweepPeriod != 0) ? pulse.sweepPeriod : 8;
		}
	}

	void triggerWave (void)
	{
		if (wave.length == 0)
		{
			wave.length = 256 - wave.lengthLoad;
		}
	}

	void triggerNoise (void)
	{
		if (noiseChannel.length == 0)
		{
			noiseChannel.length = 64 - noiseChannel.lengthLoad;
		}

		triggerEnvelope (noiseChannel.envelope);
	}

	void powerOff (void)
	{
		for (Pulse& pulse : pulses)
		{
			pulse.envelope.volume = 0;
			pulse.overflow = false;
		}
		wave.volumeCode = 0;
		noiseChannel.envelope.volume = 0;
		frameIndex = 0;
		frameAccumulator = 0.0;
	}

	void stepFrameSequencer (void)
	{
		// the frame sequencer runs at 512 Hz: of its eight steps, 0/2/4/6 clock the length
		// counters (256 Hz), 2 and 6 the sweep (128 Hz) and step 7 the envelopes (64 Hz)
		if ((frameIndex & 1) == 0)
		{
			clockLength (pulses[0].lengthEnable, pulses[0].length);
			clockLength (pulses[1].lengthEnable, pulses[1].length);
			clockLength (wave.lengthEnable, wave.length);
			clockLength (noiseChannel.lengthEnable, noiseChannel.length);
		}

		if (frameIndex == 2 || frameIndex == 6)
		{
			clockSweep (pulses[0]);
		}

		if (frameIndex == 7)
		{
			clockEnvelope (pulses[0].envelope);
			clockEnvelope (pulses[1].envelope);
			clockEnvelope (noiseChannel.envelope);
		}

		frameIndex = (frameIndex + 1) & 7;
	}

	void clockLength (bool enabled, int& length)
	{
		if (enabled && length > 0)
		{
			--length;
		}
	}

	void clockEnvelope (Envelope& envelope)
	{
		if (envelope.period == 0)
		{
			return;		// a period of 0 freezes the volume
		}

		if (envelope.timer > 0)
		{
			--envelope.timer;
			return;
		}

		envelope.timer = envelope.period;

		if (envelope.increase)
		{
			if (envelope.volume < 15)
			{
				++envelope.volume;
			}
		}
		else if (envelope.volume > 0)
		{
			--envelope.volume;
		}
	}

	void clockSweep (Pulse& pulse)
	{
		if (! pulse.sweepEnabled)
		{
			return;
		}

		if (pulse.sweepTimer > 0)
		{
			--pulse.sweepTimer;
			return;
		}

		pulse.sweepTimer = (pulse.sweepPeriod != 0) ? pulse.sweepPeriod : 8;

		const int change = pulse.frequency >> pulse.sweepShift;
		const int target = pulse.sweepNegate ? pulse.frequency - change : pulse.frequency + change;
		if (target > 2047)
		{
			pulse.overflow = true;		// the hardware silences the channel
			return;
		}

		if (pulse.sweepShift > 0)
		{
			pulse.frequency = std::max (target, 0);
		}
	}

	bool pulseAudible (const Pulse& pulse) const
	{
		return powered && pulse.envelope.dacEnabled && ! pulse.overflow && pulse.length > 0
			   && pulse.envelope.volume > 0;
	}

	bool waveAudible (void) const
	{
		return powered && wave.dacEnabled && wave.length > 0 && wave.volumeCode != 0;
	}

	bool noiseAudible (void) const
	{
		return powered && noiseChannel.envelope.dacEnabled && noiseChannel.length > 0
			   && noiseChannel.envelope.volume > 0;
	}

	int waveVolume (void) const
	{
		static const int levels[4] = { 0, 15, 11, 7 };	// mute, 100 %, 50 %, 25 %
		return levels[wave.volumeCode & 0x03];
	}
};

GbcApuDecoder::GbcApuDecoder() : impl (std::make_unique<Impl>())
{
	reset (ChipConfig { kDefaultClock, 44100.0 });
}

GbcApuDecoder::~GbcApuDecoder() = default;

void GbcApuDecoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	impl = std::make_unique<Impl>();

	// a missing clock in the header is common: fall back to the DMG's 4.19 MHz
	impl->cpuClock = (clock > 100000) ? (double) clock : (double) kDefaultClock;
	impl->sampleRate = (sampleRate > 0.0) ? sampleRate : 44100.0;
	impl->frameStep = kFrameCycles * impl->sampleRate / impl->cpuClock;

	impl->pulseTable = std::make_unique<GbToneTable> (impl->cpuClock, 32.0);
	impl->waveTable = std::make_unique<GbToneTable> (impl->cpuClock, 64.0);
	impl->noiseTable = std::make_unique<GbNoiseTable>();
}

void GbcApuDecoder::write (const RegisterWrite& w)
{
	advance (w.sampleTime);

	const uint8_t reg = w.reg;
	const uint8_t value = w.value;

	int pulseChannel = -1;
	if (reg >= 0x01 && reg <= 0x04)
	{
		pulseChannel = 0;
	}
	else if (reg >= 0x06 && reg <= 0x09)
	{
		pulseChannel = 1;
	}

	if (pulseChannel >= 0)
	{
		Impl::Pulse& pulse = impl->pulses[pulseChannel];
		// The four bytes a pulse channel owns, named the way the hardware documentation names them
		// (pulse 0 is NR11..NR14 at 0x01..0x04, pulse 1 is NR21..NR24 at 0x06..0x09):
		//
		//   0x1 / 0x6  NRx1  duty cycle in bits 7-6, length load in bits 5-0
		//   0x2 / 0x7  NRx2  envelope: initial volume, direction, period
		//   0x3 / 0x8  NRx3  frequency, low eight bits
		//   0x4 / 0x9  NRx4  trigger in bit 7, length enable in bit 6, frequency high in bits 2-0
		switch (reg & 0x0F)
		{
		case 0x01:
		case 0x06:
			pulse.duty = (value >> 6) & 0x03;
			pulse.lengthLoad = value & 0x3F;
			break;
		case 0x02:
		case 0x07:
			impl->writeEnvelope (pulse.envelope, value);
			break;
		case 0x03:
		case 0x08:
			pulse.frequency = (pulse.frequency & 0x700) | value;
			break;
		case 0x04:
		case 0x09:
			pulse.frequency = (pulse.frequency & 0x0FF) | ((value & 0x07) << 8);
			pulse.lengthEnable = (value & 0x40) != 0;
			if ((value & 0x80) != 0)
			{
				impl->triggerPulse (pulse, pulseChannel == 0);
			}
			break;
		default:
			break;
		}

		return;
	}

	// Everything outside the pulse blocks, by absolute register name:
	//
	//   0x00  NR10  pulse 0 sweep: period in bits 6-4, negate in bit 3, shift in bits 2-0
	//   0x0A  NR30  wave DAC enable (bit 7)
	//   0x0B  NR31  wave length load
	//   0x0C  NR32  wave volume code in bits 6-5
	//   0x0D  NR33  wave frequency, low eight bits
	//   0x0E  NR34  wave trigger (bit 7), length enable (bit 6), frequency high in bits 2-0
	//   0x10..0x13  NR41..NR44  noise: length, envelope, polynomial clock, trigger
	//   0x14/0x15   NR50/NR51   master volume and the routing of each channel to left/right
	//   0x16  NR52  power switch; bits 0-3 mirror whether each channel is running
	//   0x30..0x3F  wave RAM, thirty-two 4-bit samples
	switch (reg)
	{
	case 0x00:
		impl->pulses[0].sweepPeriod = (value >> 4) & 0x07;
		impl->pulses[0].sweepNegate = (value & kSweepNegate) != 0;
		impl->pulses[0].sweepShift = value & kSweepShiftMask;
		impl->pulses[0].sweepEnabled = (value & 0x7F) != 0;
		break;
	case 0x0A:
		impl->wave.dacEnabled = (value & 0x80) != 0;
		break;
	case 0x0B:
		impl->wave.lengthLoad = value;
		break;
	case 0x0C:
		impl->wave.volumeCode = (value >> 5) & 0x03;
		break;
	case 0x0D:
		impl->wave.frequency = (impl->wave.frequency & 0x700) | value;
		break;
	case 0x0E:
		impl->wave.frequency = (impl->wave.frequency & 0x0FF) | ((value & 0x07) << 8);
		impl->wave.lengthEnable = (value & 0x40) != 0;
		if ((value & 0x80) != 0)
		{
			impl->triggerWave();
		}
		break;
	case 0x10:
		impl->noiseChannel.lengthLoad = value & 0x3F;
		break;
	case 0x11:
		impl->writeEnvelope (impl->noiseChannel.envelope, value);
		break;
	case 0x12:
		impl->noiseChannel.shift = (value >> kEnvelopeVolumeShift) & kEnvelopeVolumeMask;
		impl->noiseChannel.widthMode = (value & kNoiseWidthMode) != 0;
		impl->noiseChannel.divisorCode = value & kNoiseRatioMask;
		break;
	case 0x13:
		impl->noiseChannel.lengthEnable = (value & 0x40) != 0;
		if ((value & 0x80) != 0)
		{
			impl->triggerNoise();
		}
		break;
	case 0x16:
	{
		const bool on = (value & 0x80) != 0;
		if (! on)
		{
			impl->powerOff();
			impl->powered = false;
		}
		else if (! impl->powered)
		{
			impl->powered = true;
			impl->frameIndex = 0;
			impl->frameAccumulator = 0.0;
		}

		break;
	}
	default:
		// **The wave RAM lives in the same block** at offsets 0x20-0x2F (0xFF30-0xFF3F). It changes
		// no note, but it is the whole shape of the wave channel, so it is kept for the row.
		//
		// The DMG also has a write lock here: while the channel plays, a write only lands in the
		// cycle the chip reads that byte (`gb_mame.c:278-303`). It is not mirrored - every driver in
		// this collection stops the channel before uploading, which is the only reliable sequence on
		// real hardware - so a file that did otherwise would show bytes the audio does not have.
		if (w.reg >= kWaveRamOffset && w.reg < kWaveRamOffset + kWaveBytes)
		{
			impl->waveRam[w.reg - kWaveRamOffset] = (uint8_t) w.value;
		}
		break;		// NR50/NR51 mixing: nothing a note or a shape needs
	}
}

void GbcApuDecoder::advance (uint32_t sampleTime)
{
	if (sampleTime <= impl->currentSample)
	{
		return;
	}

	const uint32_t delta = sampleTime - impl->currentSample;
	impl->currentSample = sampleTime;

	impl->frameAccumulator += (double) delta;
	while (impl->frameAccumulator >= impl->frameStep)
	{
		impl->frameAccumulator -= impl->frameStep;
		impl->stepFrameSequencer();
	}
}

ChannelState GbcApuDecoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= channelCount_)
	{
		return state;
	}

	state.present = true;

	switch (channel)
	{
	case pulse1:
	case pulse2:
	{
		const Impl::Pulse& pulse = impl->pulses[channel];
		state.gate = impl->pulseAudible (pulse);
		state.period = (uint32_t) pulse.frequency;
		state.volume = pulse.envelope.volume;
		state.patch = pulse.duty;
		break;
	}
	case wave:
		state.gate = impl->waveAudible();
		state.period = (uint32_t) impl->wave.frequency;
		state.volume = impl->waveVolume();
		state.patch = 0;

		// **The row draws the table the channel walks**, one cycle by definition, at the rate the
		// counter steps it: `clock / (2 * (2048 - f))` a second, whose thirty-second is the
		// `clock / (64 * (2048 - f))` tone the note table reports. Drawing a table even while the
		// channel is silent is deliberate - a driver loads a shape before it triggers, and a row
		// with nothing to drag is worse than a flat one.
		state.wave = impl->waveFor ();
		state.tableWave = true;
		state.sampleRate
			= (float) (impl->cpuClock
					   / (kWaveCyclesPerStep
						  * (kFrequencyRegisterModulus
							 - (double) (impl->wave.frequency & 0x07FF))));
		break;
	case noise:
		state.gate = impl->noiseAudible();
		state.period = (uint32_t) ((impl->noiseChannel.shift << 4)
										| (impl->noiseChannel.widthMode ? 0x08 : 0)
										| impl->noiseChannel.divisorCode);
		state.volume = impl->noiseChannel.envelope.volume;
		state.patch = impl->noiseChannel.widthMode ? 1 : 0;
		break;
	default:
		state.present = false;
		break;
	}

	return state;
}

const NoteTable& GbcApuDecoder::noteTable (int channel) const
{
	switch (channel)
	{
	case wave:
		return *impl->waveTable;
	case noise:
		return *impl->noiseTable;
	default:
		return *impl->pulseTable;
	}
}

ChannelKind GbcApuDecoder::channelKind (int channel) const
{
	switch (channel)
	{
	case pulse1:
	case pulse2:
		return ChannelKind::gbPulse;
	case wave:
		return ChannelKind::gbWave;
	case noise:
		return ChannelKind::gbNoise;
	default:
		return ChannelKind::unknown;
	}
}

} // namespace toybox::chips
