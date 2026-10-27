#include "Ay8910Decoder.h"

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	constexpr uint32_t kDefaultClock = 1789773;

	/** The register file, named where the reads below reach into it by position.

		Layout: 0x00/0x01 hold channel A's 12-bit divider - low byte first, then the high nibble in
		the low half of the second byte - and B and C follow in pairs, so a channel's pair starts at
		`channel * 2`. 0x07 is the mixer, where a cleared bit *enables* the matching tone (an
		inverted flag, which is why the test below reads `== 0`). 0x08..0x0A are the three
		amplitudes: bit 4 hands the channel to the envelope generator, and the low nibble is the
		level, 0x0F being full scale.
	*/
	constexpr uint8_t kMixerRegister = 0x07;
	constexpr uint8_t kFirstAmplitudeRegister = 0x08;
	constexpr uint8_t kEnvelopeFlag = 0x10;		// bit 4 of an amplitude register
	constexpr uint8_t kLevelMask = 0x0F;		// the low nibble of the same byte
	constexpr uint8_t kDividerHighMask = 0x0F;	// the divider is 12 bits: 8 below, 4 above
	constexpr int kMaxLevel = 15;

	/** A tone channel toggles every 8 clocks per divider step and needs two steps per wave, so
		the frequency is clock / (16 * period). */
	class AyToneTable final : public NoteTable
	{
	public:
		explicit AyToneTable (double clock) : clock (clock) {}

		int noteFor (uint32_t period, int, int* finePitch) const override
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
	uint8_t regs[16] = {};
	double clock = (double) kDefaultClock;
	std::unique_ptr<AyToneTable> table;
};

Ay8910Decoder::Ay8910Decoder() : impl (std::make_unique<Impl>())
{
	reset (ChipConfig { kDefaultClock, 44100.0 });
}

Ay8910Decoder::~Ay8910Decoder() = default;

void Ay8910Decoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
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

void Ay8910Decoder::advance (uint32_t sampleTime)
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

	const uint8_t amplitude = impl->regs[kFirstAmplitudeRegister + channel];
	const bool toneEnabled = (impl->regs[kMixerRegister] & (1 << channel)) == 0;
	const bool envelope = (amplitude & kEnvelopeFlag) != 0;

	state.present = true;
	state.period = (uint32_t) impl->regs[channel * 2]
				   | ((uint32_t) (impl->regs[channel * 2 + 1] & kDividerHighMask) << 8);
	state.volume = envelope ? kMaxLevel : (amplitude & kLevelMask);
	state.gate = toneEnabled && (envelope || state.volume > 0);

	return state;
}

const NoteTable& Ay8910Decoder::noteTable (int channel) const
{
	(void) channel;		// one divider relation for all three tone channels
	return *impl->table;
}

ChannelKind Ay8910Decoder::channelKind (int channel) const
{
	(void) channel;
	return ChannelKind::aySquare;
}

} // namespace toybox::chips
