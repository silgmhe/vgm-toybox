#include "K005289Decoder.h"

#include <cstdint>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	/** Thirty-two steps make one cycle of the PROM (`addr & 0x1F`, k005289.c:191). */
	constexpr double kStepsPerCycle = 32.0;

	/** The clock to assume when the VGM header carries none for this chip. */
	constexpr uint32_t kDefaultClock = 4000000;

	double frequencyOf (uint32_t clock, uint16_t divider)
	{
		return (double) clock / (kStepsPerCycle * ((double) divider + 1.0));
	}
}

struct K005289Decoder::Impl
{
	struct Voice
	{
		uint16_t latch = 0;		// the LD value, already inverted by the driver
		uint16_t freq = 0;			// what TG copied out of it - the pitch in force
		bool triggered = false;
		int volume = 0;					// 0..15; zero is silence
		int waveform = 0;
	};

	/** Both voices share one formula, so one table serves them: it needs the clock, nothing else. */
	struct Table final : NoteTable
	{
		uint32_t clock = kDefaultClock;

		int noteFor (uint32_t period, int, int* finePitch) const override
		{
			return quantiseFrequency (frequencyOf (clock, (uint16_t) period), finePitch);
		}
	};

	Table table;
	Voice voices[2];
};

K005289Decoder::K005289Decoder() = default;
K005289Decoder::~K005289Decoder() = default;

void K005289Decoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	(void) sampleRate;
	impl = std::make_unique<Impl>();
	impl->table.clock = clock > 0 ? clock : kDefaultClock;
}

void K005289Decoder::write (const RegisterWrite& write)
{
	if (impl == nullptr)
	{
		return;
	}

	const int channel = write.reg & 1;
	Impl::Voice& voice = impl->voices[channel];

	// the reader packs the command's twelve bits into one word (ChipTypes.h)
	const uint16_t data = (uint16_t) (write.value & 0x0FFF);

	switch (write.reg & 0x06)
	{
	case 0x00:		// control: volume in the low nibble, waveform number in bits 5-7
		voice.volume = data & 0x0F;
		voice.waveform = (data >> 5) & 0x07;
		break;
	case 0x02:		// LD: the driver writes the inverted divider, the core stores 0xFFF - value
		voice.latch = (uint16_t) (0x0FFF - (data & 0x0FFF));
		break;
	case 0x04:		// TG: the latched pitch becomes audible - the key-on edge
		voice.freq = voice.latch;
		voice.triggered = true;
		break;
	default:
		break;
	}
}

void K005289Decoder::advance (uint32_t)
{
	// note boundaries come from the writes; there is no envelope to step here
}

ChannelState K005289Decoder::channelState (int channel) const
{
	ChannelState state;
	if (impl == nullptr || channel < 0 || channel >= 2)
	{
		return state;
	}

	const Impl::Voice& voice = impl->voices[channel];
	state.present = true;
	state.gate = voice.triggered && voice.volume > 0;
	state.volume = voice.volume;
	state.patch = voice.waveform;
	state.period = voice.freq;
	state.hz = state.gate ? frequencyOf (impl->table.clock, voice.freq) : 0.0;
	state.pitchSource = state.gate ? PitchSource::registerExact : PitchSource::unpitched;
	return state;
}

const NoteTable& K005289Decoder::noteTable (int) const
{
	static const Impl::Table fallback;
	return impl != nullptr ? impl->table : fallback;
}

ChannelKind K005289Decoder::channelKind (int) const
{
	return ChannelKind::wavetableWave;
}

} // namespace toybox::chips
