#include "PokeyDecoder.h"

#include <algorithm>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	constexpr std::uint32_t kDefaultClock = 1789772;

	// AUDCTL
	constexpr std::uint8_t kClock15kHz = 0x01;
	constexpr std::uint8_t kChannel3High = 0x20;
	constexpr std::uint8_t kChannel1High = 0x40;
	constexpr std::uint8_t kJoin34 = 0x08;
	constexpr std::uint8_t kJoin12 = 0x10;

	// AUDC
	constexpr std::uint8_t kVolumeOnly = 0x10;

	// A POKEY divisor is not one number: with the high clock the channel counts CPU cycles and
	// the core adds four of them (seven for a joined pair), so the period has to carry the clock
	// source along with the divisor. Twelve bits of divider do not reach into these bits.
	constexpr std::uint32_t kBase64kHz = 0;
	constexpr std::uint32_t kBase1_79MHz = 1;
	constexpr std::uint32_t kBase15kHz = 2;

	std::uint32_t encodePeriod (std::uint32_t base, int divisor)
	{
		return (base << 24) | ((std::uint32_t) std::max (divisor, 1) & 0x0FFFFF);
	}

	/** frequency = clock source / (2 * divisor). The source is the CPU clock for the 1.79 MHz
		mode and its 28th or 114th part for the other two. */
	class PokeyTable final : public NoteTable
	{
	public:
		explicit PokeyTable (double clock) : clock (clock) {}

		int noteFor (std::uint32_t period, int, int* finePitch) const override
		{
			const std::uint32_t base = (period >> 24) & 0x03;
			const double divisor = (double) (period & 0x0FFFFF);
			if (divisor < 1.0)
			{
				return -1;
			}

			double source = clock / 28.0;		// the 64 kHz base clock
			if (base == kBase1_79MHz)
			{
				source = clock;
			}
			else if (base == kBase15kHz)
			{
				source = clock / 114.0;
			}

			return quantiseFrequency (source / (2.0 * divisor), finePitch);
		}

	private:
		double clock;
	};
}

struct PokeyDecoder::Impl
{
	std::uint8_t audc[numChannels] = {};	// 0x00/0x02/0x04/0x06
	std::uint8_t audf[numChannels] = {};	// 0x01/0x03/0x05/0x07
	std::uint8_t audctl = 0;				// 0x08
	double clock = (double) kDefaultClock;
	std::unique_ptr<PokeyTable> table;

	bool joinedHigh (int channel) const
	{
		return (channel == channel1 && (audctl & kJoin12) != 0)
			   || (channel == channel3 && (audctl & kJoin34) != 0);
	}

	bool joinedLow (int channel) const
	{
		return (channel == channel0 && (audctl & kJoin12) != 0)
			   || (channel == channel2 && (audctl & kJoin34) != 0);
	}

	bool highClock (int channel) const
	{
		return (channel == channel0 && (audctl & kChannel1High) != 0)
			   || (channel == channel2 && (audctl & kChannel3High) != 0);
	}

	std::uint32_t baseClockCode (int channel) const
	{
		if (highClock (channel))
		{
			return kBase1_79MHz;
		}

		return (audctl & kClock15kHz) != 0 ? kBase15kHz : kBase64kHz;
	}
};

PokeyDecoder::PokeyDecoder() : impl (std::make_unique<Impl>())
{
	reset (kDefaultClock, 44100.0);
}

PokeyDecoder::~PokeyDecoder() = default;

void PokeyDecoder::reset (std::uint32_t clock, double sampleRate)
{
	(void) sampleRate;		// no timers: the registers are the whole state

	impl = std::make_unique<Impl>();

	// the Atari 8-bit machines and the arcade boards use 1.79 MHz; some boards overclock it
	impl->clock = (clock > 100000) ? (double) clock : (double) kDefaultClock;
	impl->table = std::make_unique<PokeyTable> (impl->clock);
}

void PokeyDecoder::write (const RegisterWrite& w)
{
	const int channel = (w.reg & 0x0F) >> 1;

	if (w.reg == 0x08)
	{
		impl->audctl = w.value;
		return;
	}

	if (channel >= numChannels)
	{
		return;		// 0x09-0x0F are keyboard, serial and IRQ registers
	}

	// the chip's own order: D200 AUDF1, D201 AUDC1, D202 AUDF2 ... D208 AUDCTL
	if ((w.reg & 1) != 0)
	{
		impl->audc[channel] = w.value;
	}
	else
	{
		impl->audf[channel] = w.value;
	}
}

void PokeyDecoder::advance (std::uint32_t sampleTime)
{
	(void) sampleTime;		// no timers
}

ChannelState PokeyDecoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= channelCount_)
	{
		return state;
	}

	if (impl->joinedHigh (channel))
	{
		// the divider is the second byte of the pair's 16-bit value: not a voice of its own
		state.present = true;
		return state;
	}

	int volume = impl->audc[channel] & 0x0F;
	bool volumeOnly = (impl->audc[channel] & kVolumeOnly) != 0;
	std::uint32_t divisor = 0;
	std::uint32_t base = impl->baseClockCode (channel);

	if (impl->joinedLow (channel))
	{
		const int high = channel + 1;
		const std::uint32_t combined = (std::uint32_t) impl->audf[channel]
									   | ((std::uint32_t) impl->audf[high] << 8);

		// the pair's loudness comes from whichever channel the driver gave a volume to
		volume = std::max (volume, (int) (impl->audc[high] & 0x0F));
		volumeOnly = volumeOnly && (impl->audc[high] & kVolumeOnly) != 0;
		divisor = combined + ((base == kBase1_79MHz) ? 7u : 1u);
	}
	else
	{
		divisor = (std::uint32_t) impl->audf[channel] + ((base == kBase1_79MHz) ? 4u : 1u);
	}

	state.present = true;
	state.period = encodePeriod (base, (int) divisor);
	state.volume = volume;
	state.gate = volume > 0 && ! volumeOnly;
	state.patch = (impl->audc[channel] >> 5) & 0x07;		// the distortion selector

	return state;
}

const NoteTable& PokeyDecoder::noteTable (int channel) const
{
	(void) channel;		// the clock source travels in the period, one table serves all four
	return *impl->table;
}

Family PokeyDecoder::family (int channel) const
{
	(void) channel;
	return Family::pokeySquare;
}

} // namespace toybox::chips
