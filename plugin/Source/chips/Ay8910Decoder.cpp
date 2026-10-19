#include "Ay8910Decoder.h"

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	constexpr std::uint32_t kDefaultClock = 1789773;

	/** A tone channel toggles every 8 clocks per divider step and needs two steps per wave, so
		the frequency is clock / (16 * period). */
	class AyToneTable final : public NoteTable
	{
	public:
		explicit AyToneTable (double clock) : clock (clock) {}

		int noteFor (std::uint32_t period, int, int* finePitch) const override
		{
			if (period == 0)
			{
				return -1;
			}

			return quantiseFrequency (clock / (16.0 * (double) period), finePitch);
		}

	private:
		double clock;
	};
}

struct Ay8910Decoder::Impl
{
	std::uint8_t regs[16] = {};
	double clock = (double) kDefaultClock;
	std::unique_ptr<AyToneTable> table;
};

Ay8910Decoder::Ay8910Decoder() : impl (std::make_unique<Impl>())
{
	reset (kDefaultClock, 44100.0);
}

Ay8910Decoder::~Ay8910Decoder() = default;

void Ay8910Decoder::reset (std::uint32_t clock, double sampleRate)
{
	(void) sampleRate;		// no timers: the registers are the whole state

	impl = std::make_unique<Impl>();

	impl->clock = (clock > 100000) ? (double) clock : (double) kDefaultClock;
	impl->table = std::make_unique<AyToneTable> (impl->clock);
}

void Ay8910Decoder::write (const RegisterWrite& w)
{
	if (w.reg < 16)
	{
		impl->regs[w.reg] = w.value;
	}
}

void Ay8910Decoder::advance (std::uint32_t sampleTime)
{
	(void) sampleTime;		// no timers
}

ChannelState Ay8910Decoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= channelCount_)
	{
		return state;
	}

	const std::uint8_t amplitude = impl->regs[0x08 + channel];
	const bool toneEnabled = (impl->regs[0x07] & (1 << channel)) == 0;
	const bool envelope = (amplitude & 0x10) != 0;

	state.present = true;
	state.period = (std::uint32_t) impl->regs[channel * 2]
				   | ((std::uint32_t) (impl->regs[channel * 2 + 1] & 0x0F) << 8);
	state.volume = envelope ? 15 : (amplitude & 0x0F);
	state.gate = toneEnabled && (envelope || state.volume > 0);

	return state;
}

const NoteTable& Ay8910Decoder::noteTable (int channel) const
{
	(void) channel;		// one divider relation for all three tone channels
	return *impl->table;
}

Family Ay8910Decoder::family (int channel) const
{
	(void) channel;
	return Family::aySquare;
}

} // namespace toybox::chips
