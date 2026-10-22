#include "K051649Decoder.h"

#include <algorithm>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	constexpr std::uint32_t kDefaultClock = 3579545;

	// the core stops a channel at this frequency: nine clocks cannot fill 32 waveform samples
	constexpr int kMinimumFrequency = 8;

	/** One wave cycle is 32 samples, each one clocked after 16 master clocks, and the register is
		the number of clocks between two samples minus one: clock / (16 * (register + 1)). */
	class SccTable final : public NoteTable
	{
	public:
		explicit SccTable (double clock) : clock (clock) {}

		int noteFor (std::uint32_t period, int, int* finePitch) const override
		{
			if (period > 0x0FFF)
			{
				return -1;
			}

			return quantiseFrequency (clock / (16.0 * ((double) period + 1.0)), finePitch);
		}

	private:
		double clock;
	};
}

struct K051649Decoder::Impl
{
	int frequency[numChannels] = {};
	std::uint8_t volume[numChannels] = {};
	bool key[numChannels] = {};
	std::uint8_t latch = 0;
	std::uint8_t test = 0;
	double clock = (double) kDefaultClock;
	std::unique_ptr<SccTable> table;
};

K051649Decoder::K051649Decoder() : impl (std::make_unique<Impl>())
{
	reset (kDefaultClock, 44100.0);
}

K051649Decoder::~K051649Decoder() = default;

void K051649Decoder::reset (std::uint32_t clock, double sampleRate)
{
	(void) sampleRate;		// no timers: the key mask is the gate

	impl = std::make_unique<Impl>();

	impl->clock = (clock > 100000) ? (double) clock : (double) kDefaultClock;
	impl->table = std::make_unique<SccTable> (impl->clock);
}

void K051649Decoder::write (const RegisterWrite& w)
{
	// even offsets set the register index, the odd one after it carries the value
	if ((w.reg & 1) == 0)
	{
		impl->latch = w.value;
		return;
	}

	const std::uint8_t value = w.value;

	// The SCC's register halves, after the latch (the address is `reg >> 1`, which is why the
	// labels below are 0x00..0x05 rather than byte offsets):
	//
	//   0x00  waveform RAM          0x04  the same RAM, addressed rotated in SCC+ mode
	//   0x01  frequency: the latched address selects a channel and which half of the period
	//         (bit 0 = high bits), so one register number carries two different writes
	//   0x02  volume and channel enable, same latched selection
	//   0x03  key mask: the data byte says which channels run, the address only latches
	//   0x05  test register (waveform deformation), not a note
	switch (w.reg >> 1)
	{
	case 0x00:		// waveform data
	case 0x04:		// the same RAM, addressed rotated in SCC+ mode
		break;
	case 0x01:
	{
		const int channel = (impl->latch >> 1) & 0x07;
		if (channel >= numChannels)
		{
			break;
		}

		if ((impl->latch & 1) != 0)
		{
			impl->frequency[channel] = (impl->frequency[channel] & 0x0FF) | ((value & 0x0F) << 8);
		}
		else
		{
			impl->frequency[channel] = (impl->frequency[channel] & 0xF00) | value;
		}

		break;
	}
	case 0x02:
	{
		const int channel = impl->latch & 0x07;
		if (channel < numChannels)
		{
			impl->volume[channel] = (std::uint8_t) (value & 0x0F);
		}

		break;
	}
	case 0x03:
		// the key mask is the data byte; the address only carries the latch
		for (int channel = 0; channel < numChannels; ++channel)
		{
			impl->key[channel] = (value & (1 << channel)) != 0;
		}
		break;
	case 0x05:
		impl->test = value;
		break;
	default:
		break;
	}
}

void K051649Decoder::advance (std::uint32_t sampleTime)
{
	(void) sampleTime;		// no timers
}

ChannelState K051649Decoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= channelCount_)
	{
		return state;
	}

	state.present = true;
	state.period = (std::uint32_t) impl->frequency[channel];
	state.volume = impl->volume[channel];
	state.gate = impl->key[channel] && state.volume > 0
				 && impl->frequency[channel] > kMinimumFrequency;
	state.patch = 0;

	return state;
}

const NoteTable& K051649Decoder::noteTable (int channel) const
{
	(void) channel;		// one divider for all five channels
	return *impl->table;
}

Family K051649Decoder::family (int channel) const
{
	(void) channel;
	return Family::sccWave;
}

} // namespace toybox::chips
