#include "X1_010Decoder.h"

#include <algorithm>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	// Seta boards run the X1-010 at 16 MHz; the header clock is used when it is there.
	constexpr std::uint32_t kDefaultClock = 16000000;

	constexpr int kOneShotNote = 60;
	constexpr std::uint32_t kOneShotPeriod = 0x80000000u;

	// A PCM sample ends where its end register says, and that register points into the sample data
	// rather than naming a time - so the gate is a fixed window, like the OPN's ADPCM-A.
	constexpr std::uint32_t kPcmGateSamples = 3528;		// 80 ms at 44.1 kHz

	// One wavetable cycle is 128 bytes and the core steps the table clock * pitch / 2^26 times a
	// second (x1_010.c: step = base_clock / 524288 * freq, stepped through 128 samples).
	constexpr double kCycleDivisor = 67108864.0;

	class X1NoteTable final : public NoteTable
	{
	public:
		explicit X1NoteTable (double clock) : clock (clock) {}

		int noteFor (std::uint32_t period, int, int* finePitch) const override
		{
			if ((period & kOneShotPeriod) != 0)
			{
				if (finePitch != nullptr)
				{
					*finePitch = 0;
				}

				return kOneShotNote;
			}

			return quantiseFrequency (clock * (double) period / kCycleDivisor, finePitch);
		}

	private:
		double clock;
	};
}

struct X1_010Decoder::Impl
{
	struct Channel
	{
		std::uint8_t status = 0;	// 0: bit 7 divider, bit 2 repeat, bit 1 waveform, bit 0 key on
		std::uint8_t volume = 0;	// 1: PCM L/R nibbles, or the waveform number in waveform mode
		std::uint8_t pitchLo = 0;	// 2
		std::uint8_t pitchHi = 0;	// 3
		std::uint32_t gateEnd = 0;	// PCM: the one-shot window closes here
		bool gateOpen = false;
	};

	Channel channels[numChannels];
	std::uint32_t now = 0;
	double clock = (double) kDefaultClock;
	std::unique_ptr<X1NoteTable> table;

	bool isWaveform (int channel) const { return (channels[channel].status & 0x02) != 0; }
	bool isKeyedOn (int channel) const { return (channels[channel].status & 0x01) != 0; }

	/** 14-bit 6.10 pitch, halved by the divider flag - the same shift the core applies. */
	std::uint32_t pitchOf (int channel) const
	{
		const Channel& current = channels[channel];
		std::uint32_t pitch = ((std::uint32_t) current.pitchHi << 8) | current.pitchLo;
		if ((current.status & 0x80) != 0)
		{
			pitch >>= 1;
		}

		return pitch;
	}
};

X1_010Decoder::X1_010Decoder() : impl (std::make_unique<Impl>())
{
	reset (ChipConfig { kDefaultClock, 44100.0 });
}

X1_010Decoder::~X1_010Decoder() = default;

void X1_010Decoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const std::uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	(void) sampleRate;		// the gate is counted in 44.1 kHz samples, the chip has no other time base

	impl = std::make_unique<Impl>();
	impl->clock = (clock > 100000) ? (double) clock : (double) kDefaultClock;
	impl->table = std::make_unique<X1NoteTable> (impl->clock);
}

void X1_010Decoder::write (const RegisterWrite& w)
{
	// The VGM command carries a 16-bit offset; the channel registers are the low 0x80 of it and
	// everything above is envelope or wavetable data, which is uploaded wholesale and says nothing
	// about which note is playing.
	const int reg = ((int) w.port << 8) | (int) w.reg;
	if (reg < 0 || reg >= 0x80)
	{
		return;
	}

	const int channel = reg >> 3;
	const int field = reg & 0x07;
	Impl::Channel& current = impl->channels[channel];

	switch (field)
	{
	case 0:
	{
		const bool wasOn = (current.status & 0x01) != 0;
		current.status = w.value;

		const bool isOn = (current.status & 0x01) != 0;
		const bool waveform = (current.status & 0x02) != 0;
		if (! isOn)
		{
			current.gateOpen = false;
		}
		else if (! wasOn && ! waveform)
		{
			current.gateEnd = w.sampleTime + kPcmGateSamples;
			current.gateOpen = true;
		}

		break;
	}
	case 1:
		current.volume = w.value;
		break;
	case 2:
		current.pitchLo = w.value;
		break;
	case 3:
		current.pitchHi = w.value;
		break;
	default:
		break;		// 4/5 start/end (PCM) or envelope time/number, 6/7 reserved
	}
}

void X1_010Decoder::advance (std::uint32_t sampleTime)
{
	impl->now = sampleTime;

	for (auto& channel : impl->channels)
	{
		if (channel.gateOpen && sampleTime >= channel.gateEnd)
		{
			channel.gateOpen = false;
		}
	}
}

ChannelState X1_010Decoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= channelCount_)
	{
		return state;
	}

	const bool waveform = impl->isWaveform (channel);
	state.present = true;
	state.patch = impl->channels[channel].volume;

	if (waveform)
	{
		state.gate = impl->isKeyedOn (channel);
		state.period = impl->pitchOf (channel);
		state.volume = 15;		// the envelope registers shape the volume; not modelled
	}
	else
	{
		state.gate = impl->channels[channel].gateOpen;
		state.period = kOneShotPeriod;
		state.volume = 15;
	}

	return state;
}

const NoteTable& X1_010Decoder::noteTable (int channel) const
{
	(void) channel;		// one table: PCM hands it the one-shot marker, waveform a real pitch
	return *impl->table;
}

Family X1_010Decoder::family (int channel) const
{
	return impl->isWaveform (channel) ? Family::x1Wave : Family::adpcmDrum;
}

bool X1_010Decoder::isDrum (int channel) const
{
	return ! impl->isWaveform (channel);
}

} // namespace toybox::chips
