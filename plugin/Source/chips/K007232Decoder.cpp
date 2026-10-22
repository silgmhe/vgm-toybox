#include "K007232Decoder.h"

#include <cstdint>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	/** The core runs at clock / 128 (`K007232_CLOCKDIV`, k007232.c:50). */
	constexpr double kClockDivisor = 128.0;

	/** The counter walks thirty-two per output sample and reloads with `0x1000 - step`. */
	constexpr double kCounterWalk = 32.0;
	constexpr double kCounterRange = 4096.0;

	constexpr std::uint32_t kDefaultClock = 8000000;

	double frequencyOf (std::uint32_t clock, std::uint16_t step)
	{
		// steps per sample = 32 / (4096 - step); the core's sample rate is clock / 128
		const double remaining = kCounterRange - (double) step;
		if (remaining <= 0.0)
		{
			return 0.0;		// step at the top of the range never advances: not a pitch
		}

		return (double) clock / (kClockDivisor / kCounterWalk * remaining);
	}
}

struct K007232Decoder::Impl
{
	struct Voice
	{
		std::uint8_t regs[6] = { 0, 0, 0, 0, 0, 0 };
		std::uint16_t step = 0;
		bool keyed = false;
	};

	/** Both voices share the formula, so one table serves them. */
	struct Table final : NoteTable
	{
		std::uint32_t clock = kDefaultClock;

		int noteFor (std::uint32_t period, int, int* finePitch) const override
		{
			return quantiseFrequency (frequencyOf (clock, (std::uint16_t) period), finePitch);
		}
	};

	Table table;
	Voice voices[2];
};

K007232Decoder::K007232Decoder() = default;
K007232Decoder::~K007232Decoder() = default;

void K007232Decoder::reset (std::uint32_t clock, double sampleRate)
{
	(void) sampleRate;
	impl = std::make_unique<Impl>();
	impl->table.clock = clock > 0 ? clock : kDefaultClock;
}

void K007232Decoder::write (const RegisterWrite& write)
{
	if (impl == nullptr || write.reg >= 12)
	{
		return;
	}

	const int channel = write.reg / 6;
	const int offset = write.reg % 6;
	Impl::Voice& voice = impl->voices[channel];
	voice.regs[offset] = write.value;

	switch (offset)
	{
	case 0x00:		// pitch, low byte
	case 0x01:		// pitch, high byte - the low nibble is the step's top bits
	{
		const std::uint8_t low = voice.regs[0];
		const std::uint8_t high = voice.regs[1];
		const std::uint8_t mode = (std::uint8_t) ((high >> 4) & 0x03);

		// only the normal twelve-bit mode is real; the core keeps the other two as placeholders and
		// falls back to the same arithmetic, so this mirrors it rather than inventing behaviour
		voice.step = (std::uint16_t) (((high & 0x0F) << 8) | low);
		if (mode == 0x01)
		{
			voice.step = (std::uint16_t) ((256 - low) << 4);
		}
		else if (mode == 0x02)
		{
			voice.step = (std::uint16_t) ((16 - (high & 0x0F)) << 8);
		}
		break;
	}
	case 0x04:		// writing the start address is what starts a voice here: not one of the six rips ever
		voice.keyed = true;		// writes the key register 0x05, so keying on that left every channel silent
		break;
	case 0x05:		// key on/off: bit 0 is the key, for the drivers that do use it
		voice.keyed = (write.value & 0x01) != 0;
		break;
	default:
		break;		// 0x02..0x04 are the start address, which the pitch does not use
	}
}

void K007232Decoder::advance (std::uint32_t)
{
	// the writes carry every boundary; nothing to step
}

ChannelState K007232Decoder::channelState (int channel) const
{
	ChannelState state;
	if (impl == nullptr || channel < 0 || channel >= 2)
	{
		return state;
	}

	const Impl::Voice& voice = impl->voices[channel];
	state.present = true;
	state.period = voice.step;
	state.gate = voice.keyed && voice.step > 0;
	state.volume = state.gate ? 15 : 0;
	state.hz = state.gate ? frequencyOf (impl->table.clock, voice.step) : 0.0;
	state.pitchSource = state.gate ? PitchSource::registerExact : PitchSource::unpitched;
	return state;
}

const NoteTable& K007232Decoder::noteTable (int) const
{
	static const Impl::Table fallback;
	return impl != nullptr ? impl->table : fallback;
}

Family K007232Decoder::family (int) const
{
	return Family::k053260Pcm;		// the same shape: a PCM voice with a rate register
}

} // namespace toybox::chips
