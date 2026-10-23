#include "NesApuDecoder.h"

#include <algorithm>
#include <cmath>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	/** Pulse and triangle: frequency = cpu / (divider * (period + 1)). */
	class NesPeriodTable final : public NoteTable
	{
	public:
		NesPeriodTable (double cpu, double divisor) : cpuClock (cpu), divider (divisor) {}

		int noteFor (std::uint32_t period, int, int* finePitch) const override
		{
			return quantiseFrequency (cpuClock / (divider * ((double) period + 1.0)), finePitch);
		}

	private:
		double cpuClock;
		double divider;
	};

	/** The noise channel has no musical pitch: its 16 period indices are mapped onto GM drum
		keys so the track stays usable instead of turning into a random melody. */
	class NesNoiseTable final : public NoteTable
	{
	public:
		int noteFor (std::uint32_t period, int, int* finePitch) const override
		{
			static const int keys[] = { 38, 38, 39, 39, 40, 40, 41, 41,
										42, 42, 43, 44, 45, 46, 47, 48 };
			const int index = (int) std::min<std::uint32_t> (period, 15);
			if (finePitch != nullptr)
			{
				*finePitch = 0;
			}

			return keys[index];
		}
	};

	class DmcTable final : public NoteTable
	{
	public:
		int noteFor (std::uint32_t, int, int* finePitch) const override
		{
			if (finePitch != nullptr)
			{
				*finePitch = 0;
			}

			return 35;	// GM acoustic bass drum - the DMC is a sample channel
		}
	};

	// NTSC 128/64/32/16-point noise period table, in CPU cycles
	const std::uint16_t kNoisePeriods[16] = { 4, 8, 16, 32, 64, 96, 128, 160,
											  202, 254, 380, 508, 762, 1016, 2034, 4068 };

	// DMC rate table: CPU cycles per output byte
	const std::uint16_t kDmcRates[16] = { 428, 380, 340, 320, 286, 254, 226, 214,
										 190, 160, 142, 128, 106, 85, 72, 54 };

	// length counter load values, in half frames: the register holds the index, not the count
	const std::uint8_t kLengthTable[32] = { 10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
											12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30 };
}

struct NesApuDecoder::Impl
{
	Pulse pulses[2];
	Triangle tri;
	NoiseChannel noiseChannel;
	Envelope dpcm {};
	std::uint8_t channelEnabled = 0;
	bool dpcmEnabled = false;
	int dpcmBytes = 0;
	double dpcmFraction = 0.0;
	int dpcmRateIndex = 0;
	bool dpcmLoop = false;

	double cpuClock = 1789773.0;
	double sampleRate = 44100.0;
	double frameStep = 7457.0;		// samples between frame sequencer clocks
	double frameAccumulator = 0.0;
	int frameCounter = 0;			// 0..3 / 0..4
	bool fiveStep = false;

	std::uint32_t currentSample = 0;

	std::unique_ptr<NoteTable> pulseTable;
	std::unique_ptr<NoteTable> triangleTable;
	std::unique_ptr<NoteTable> noiseTable;
	std::unique_ptr<NoteTable> dmcTable;

	int effectiveVolume (const Envelope& envelope) const
	{
		return envelope.constant ? envelope.volume : envelope.decay;
	}

	bool pulseActive (const Pulse& pulse) const
	{
		return ! pulse.muted && pulse.length.value > 0 && effectiveVolume (pulse.envelope) > 0
			   && pulse.timerPeriod >= 8;
	}

	bool triangleActive (void) const
	{
		return tri.length.value > 0 && tri.linearCounter > 0;
	}

	bool noiseActive (void) const
	{
		return noiseChannel.length.value > 0 && effectiveVolume (noiseChannel.envelope) > 0;
	}

	double noiseFrequency (void) const
	{
		if (noiseChannel.timerPeriod <= 0)
		{
			return 0.0;
		}

		return cpuClock / (2.0 * (double) noiseChannel.timerPeriod);
	}
};

NesApuDecoder::NesApuDecoder() : impl (std::make_unique<Impl>())
{
	reset (ChipConfig { 1789773, 44100.0 });
}

NesApuDecoder::~NesApuDecoder() = default;

void NesApuDecoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const std::uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	impl = std::make_unique<Impl>();

	// a missing clock in the header is common: fall back to the NTSC CPU rate
	impl->cpuClock = (clock > 1000000) ? (double) clock : 1789773.0;
	impl->sampleRate = (sampleRate > 0.0) ? sampleRate : 44100.0;

	// the sequencer is clocked every 7457 (NTSC) / 8313 (PAL) CPU cycles - converted to samples
	const bool pal = impl->cpuClock < 1700000.0;
	impl->frameStep = (pal ? 8313.0 : 7457.0) * impl->sampleRate / impl->cpuClock;

	impl->pulseTable = std::make_unique<NesPeriodTable> (impl->cpuClock, 16.0);
	impl->triangleTable = std::make_unique<NesPeriodTable> (impl->cpuClock, 32.0);
	impl->noiseTable = std::make_unique<NesNoiseTable>();
	impl->dmcTable = std::make_unique<DmcTable>();

	impl->pulses[0].envelope.loop = true;
	impl->pulses[1].envelope.loop = true;
	impl->noiseChannel.envelope.loop = true;
	impl->tri.linearCounter = 0;
}

void NesApuDecoder::write (const RegisterWrite& w)
{
	advance (w.sampleTime);

	const std::uint8_t reg = w.reg;
	const std::uint8_t value = w.value;

	if (reg >= 0x00 && reg <= 0x07)
	{
		Pulse& pulse = impl->pulses[(reg >> 2) & 1];
		switch (reg & 0x03)
		{
		case 0:
			pulse.duty = (value >> 6) & 0x03;
			pulse.envelope.loop = (value & 0x20) != 0;
			pulse.envelope.constant = (value & 0x10) != 0;
			pulse.envelope.volume = value & 0x0F;
			pulse.length.halt = (value & 0x20) != 0;
			break;
		case 1:
			pulse.sweep.enabled = (value & 0x80) != 0;
			pulse.sweep.period = (value >> 4) & 0x07;
			pulse.sweep.negate = (value & 0x08) != 0;
			pulse.sweep.shift = value & 0x07;
			pulse.sweep.reload = true;
			break;
		case 2:
			pulse.timerPeriod = (pulse.timerPeriod & 0x700) | value;
			break;
		case 3:
			pulse.timerPeriod = (pulse.timerPeriod & 0x0FF) | ((value & 0x07) << 8);
			pulse.length.value = kLengthTable[(value >> 3) & 0x1F];
			pulse.envelope.start = true;
			pulse.sweep.reload = true;
			break;
		default:
			break;
		}

		return;
	}

	switch (reg)
	{
	// The APU registers this switch owns, by their 6502 address minus $4000 (the pulse channels'
	// $4000..$4007 are handled above, and $4015/$4017 are the shared control registers):
	//
	//   0x08  $4008  triangle linear counter: bit 7 control, bits 6-0 the reload
	//   0x0A/0x0B  $400A/$400B  triangle period, low byte then three high bits
	//   0x0C  $400C  noise envelope: bit 5 loop, bit 4 constant volume, bits 3-0 volume/period
	//   0x0E  $400E  noise mode (bit 7 picks the short LFSR) and period index in bits 3-0
	//   0x0F  $400F  noise length load, bits 7-3
	//   0x10  $4010  DPCM control: bit 6 loop, bit 0 IRQ enable, bits 3-0 rate index
	//   0x11  $4011  DPCM direct load - the output level, not a note
	//   0x12  $4012  DPCM sample address
	//   0x13  $4013  DPCM sample length, in sixteen-byte units
	//   0x15  $4015  status: writing it enables or disables each of the five channels
	//   0x17  $4017  frame counter: bit 7 five-step sequence, bit 6 IRQ inhibit
	case 0x08:
		impl->tri.control = (value & 0x80) != 0;
		impl->tri.linearReload = value & 0x7F;
		impl->tri.length.halt = (value & 0x80) != 0;
		break;
	case 0x0A:
		impl->tri.timerPeriod = (impl->tri.timerPeriod & 0x700) | value;
		break;
	case 0x0B:
		impl->tri.timerPeriod = (impl->tri.timerPeriod & 0x0FF) | ((value & 0x07) << 8);
		impl->tri.length.value = kLengthTable[(value >> 3) & 0x1F];
		impl->tri.reloadFlag = true;
		break;
	case 0x0C:
		impl->noiseChannel.envelope.loop = (value & 0x20) != 0;
		impl->noiseChannel.envelope.constant = (value & 0x10) != 0;
		impl->noiseChannel.envelope.volume = value & 0x0F;
		impl->noiseChannel.length.halt = (value & 0x20) != 0;
		break;
	case 0x0E:
		impl->noiseChannel.mode = (value & 0x80) != 0;
		impl->noiseChannel.periodIndex = value & 0x0F;
		impl->noiseChannel.timerPeriod = kNoisePeriods[impl->noiseChannel.periodIndex];
		break;
	case 0x0F:
		impl->noiseChannel.length.value = kLengthTable[(value >> 3) & 0x1F];
		impl->noiseChannel.envelope.start = true;
		break;
	case 0x10:
		impl->dpcmLoop = (value & 0x40) != 0;
		impl->dpcmRateIndex = value & 0x0F;
		break;
	case 0x11:
		break;	// direct load: the DPCM output level, not a note
	case 0x12:
		break;	// sample address, only meaningful with a full CPU model
	case 0x13:
		impl->dpcmBytes = (value == 0) ? 1 : ((int) value * 16 + 1);
		break;
	case 0x15:
	{
		const std::uint8_t wasEnabled = impl->channelEnabled;
		impl->channelEnabled = value & 0x1F;

		// clearing a bit while written 0 zeroes that channel's length counter: this is how a
		// driver keys a note off, the length registers themselves can only reload
		if ((wasEnabled & 0x01) != 0 && (value & 0x01) == 0)
		{
			impl->pulses[0].length.value = 0;
		}
		if ((wasEnabled & 0x02) != 0 && (value & 0x02) == 0)
		{
			impl->pulses[1].length.value = 0;
		}
		if ((wasEnabled & 0x04) != 0 && (value & 0x04) == 0)
		{
			impl->tri.length.value = 0;
		}
		if ((wasEnabled & 0x08) != 0 && (value & 0x08) == 0)
		{
			impl->noiseChannel.length.value = 0;
		}

		impl->dpcmEnabled = (value & 0x10) != 0;
		if (! impl->dpcmEnabled)
		{
			impl->dpcmBytes = 0;
		}

		break;
	}
	case 0x17:
		impl->fiveStep = (value & 0x80) != 0;
		impl->frameCounter = 0;
		impl->frameAccumulator = 0.0;
		break;
	default:
		break;
	}
}

void NesApuDecoder::advance (std::uint32_t sampleTime)
{
	if (sampleTime <= impl->currentSample)
	{
		return;
	}

	const std::uint32_t delta = sampleTime - impl->currentSample;
	impl->currentSample = sampleTime;

	// the DMC is a byte-stream channel: its bytes are consumed at a rate fixed by the CPU
	if (impl->dpcmBytes > 0 && impl->dpcmEnabled)
	{
		const double cycles = (double) delta * (impl->cpuClock / impl->sampleRate);
		impl->dpcmFraction += cycles / (double) kDmcRates[impl->dpcmRateIndex & 0x0F];
		const int consumed = (int) impl->dpcmFraction;
		if (consumed > 0)
		{
			impl->dpcmFraction -= (double) consumed;
			impl->dpcmBytes -= consumed;
			if (impl->dpcmBytes <= 0)
			{
				impl->dpcmBytes = impl->dpcmLoop ? 1 : 0;
			}
		}
	}

	impl->frameAccumulator += (double) delta;
	while (impl->frameAccumulator >= impl->frameStep)
	{
		impl->frameAccumulator -= impl->frameStep;
		stepFrameSequencer();
	}
}

void NesApuDecoder::stepFrameSequencer (void)
{
	const int step = impl->frameCounter;
	const bool halfFrame = step == 1 || step == 3 || (impl->fiveStep && step == 4);

	// the envelope is clocked on every frame sequencer step
	clockEnvelope (impl->pulses[0].envelope);
	clockEnvelope (impl->pulses[1].envelope);
	clockEnvelope (impl->noiseChannel.envelope);

	if (halfFrame)
	{
		clockLength (impl->pulses[0].length);
		clockLength (impl->pulses[1].length);
		clockLength (impl->tri.length);
		clockLength (impl->noiseChannel.length);
		clockSweep (impl->pulses[0]);
		clockSweep (impl->pulses[1]);
	}

	if (step == 0)
	{
		// quarter frame: the triangle's linear counter
		if (impl->tri.reloadFlag)
		{
			impl->tri.linearCounter = impl->tri.linearReload;
		}
		else if (impl->tri.linearCounter > 0)
		{
			--impl->tri.linearCounter;
		}

		if (! impl->tri.control)
		{
			impl->tri.reloadFlag = false;
		}
	}

	impl->frameCounter = (step + 1) % (impl->fiveStep ? 5 : 4);
}

void NesApuDecoder::clockEnvelope (Envelope& envelope)
{
	if (envelope.start)
	{
		envelope.start = false;
		envelope.decay = 15;
		envelope.divider = envelope.volume;
		return;
	}

	if (envelope.divider > 0)
	{
		--envelope.divider;
		return;
	}

	envelope.divider = envelope.volume;
	if (envelope.decay > 0)
	{
		--envelope.decay;
	}
	else if (envelope.loop)
	{
		envelope.decay = 15;
	}
}

void NesApuDecoder::clockLength (LengthCounter& length)
{
	if (! length.halt && length.value > 0)
	{
		--length.value;
	}
}

void NesApuDecoder::clockSweep (Pulse& pulse)
{
	Sweep& sweep = pulse.sweep;

	if (sweep.divider == 0 && sweep.enabled && sweep.shift > 0 && ! pulse.muted)
	{
		const int change = pulse.timerPeriod >> sweep.shift;
		const int target = sweep.negate ? pulse.timerPeriod - change : pulse.timerPeriod + change;
		if (target > 0x7FF)
		{
			pulse.muted = true;	// the hardware silences the channel
		}
		else if (pulse.timerPeriod >= 8 && target >= 8)
		{
			pulse.timerPeriod = target;
		}
	}

	if (sweep.divider == 0 || sweep.reload)
	{
		sweep.divider = sweep.period;
		sweep.reload = false;
	}
	else
	{
		--sweep.divider;
	}

	if (pulse.timerPeriod < 8)
	{
		pulse.muted = true;
	}
}

ChannelState NesApuDecoder::channelState (int channel) const
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
		const Pulse& pulse = impl->pulses[channel];
		state.gate = impl->pulseActive (pulse);
		state.period = (std::uint32_t) pulse.timerPeriod;
		state.volume = impl->effectiveVolume (pulse.envelope);
		state.patch = pulse.duty;
		break;
	}
	case triangle:
		state.gate = impl->triangleActive();
		state.period = (std::uint32_t) impl->tri.timerPeriod;
		state.volume = 15;
		state.patch = 1;
		break;
	case noise:
		state.gate = impl->noiseActive();
		state.period = (std::uint32_t) impl->noiseChannel.periodIndex;
		state.volume = impl->effectiveVolume (impl->noiseChannel.envelope);
		state.patch = impl->noiseChannel.mode ? 1 : 0;
		break;
	case dmc:
		state.gate = impl->dpcmEnabled && impl->dpcmBytes > 0;
		state.period = 0;
		state.volume = 15;
		break;
	default:
		state.present = false;
		break;
	}

	return state;
}

const NoteTable& NesApuDecoder::noteTable (int channel) const
{
	switch (channel)
	{
	case triangle:
		return *impl->triangleTable;
	case noise:
		return *impl->noiseTable;
	case dmc:
		return *impl->dmcTable;
	default:
		return *impl->pulseTable;
	}
}

ChannelKind NesApuDecoder::channelKind (int channel) const
{
	switch (channel)
	{
	case pulse1:
	case pulse2:
		return ChannelKind::nesPulse;
	case triangle:
		return ChannelKind::nesTriangle;
	case noise:
		return ChannelKind::nesNoise;
	case dmc:
		return ChannelKind::nesDmc;
	default:
		return ChannelKind::unknown;
	}
}

} // namespace toybox::chips
