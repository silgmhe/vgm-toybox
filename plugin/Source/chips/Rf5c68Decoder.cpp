#include "Rf5c68Decoder.h"

namespace toybox::chips
{
namespace
{
	constexpr std::uint32_t kDefaultClock = 12500000;

	// a marker, not a measured pitch: the sample data is not in the register stream
	constexpr int kOneShotNote = 60;
	constexpr std::uint32_t kOneShotPeriod = 0x80000000u;

	constexpr std::uint32_t kGateSamples = 3528;		// 80 ms at 44.1 kHz

	class Rf5cNoteTable final : public NoteTable
	{
	public:
		int noteFor (std::uint32_t period, int, int* finePitch) const override
		{
			if (finePitch != nullptr)
			{
				*finePitch = 0;
			}

			return (period & kOneShotPeriod) != 0 ? kOneShotNote : -1;
		}
	};
}

struct Rf5c68Decoder::Impl
{
	bool master = true;					// 0x07 bit 7: sound output enable
	std::uint8_t channelMask = 0xFF;	// 0x08, a cleared bit enables the channel
	bool sounding[numChannels] = {};
	std::uint32_t gateEnd[numChannels] = {};
	std::uint32_t now = 0;
	Rf5cNoteTable table;
};

Rf5c68Decoder::Rf5c68Decoder() : impl (std::make_unique<Impl>())
{
	reset (kDefaultClock, 44100.0);
}

Rf5c68Decoder::~Rf5c68Decoder() = default;

void Rf5c68Decoder::reset (std::uint32_t clock, double sampleRate)
{
	(void) clock;			// the gate is a fixed time, so the master clock does not enter
	(void) sampleRate;

	impl = std::make_unique<Impl>();
}

void Rf5c68Decoder::write (const RegisterWrite& w)
{
	// bit i cleared enables channel i; the driver decides when the sample stops
	const auto applyChannelMask = [this, &w] (void)
	{
		if (! impl->master)
		{
			return;
		}

		for (int channel = 0; channel < channelCount_; ++channel)
		{
			const bool on = ((impl->channelMask >> channel) & 1) == 0;
			if (on && ! impl->sounding[channel])
			{
				impl->gateEnd[channel] = w.sampleTime + kGateSamples;
			}

			impl->sounding[channel] = on;
		}
	};

	if (w.reg == 0x07)
	{
		const bool master = (w.value & 0x80) != 0;
		if (master && ! impl->master)
		{
			// the core resumes from the channel's current address, so re-enabling the output
			// restarts the sample: the marker is retriggered rather than left running
			for (bool& sounding : impl->sounding)
			{
				sounding = false;
			}

			impl->master = true;
			applyChannelMask();
		}
		else if (! master)
		{
			for (bool& sounding : impl->sounding)
			{
				sounding = false;
			}

			impl->master = false;
		}
		return;
	}

	if (w.reg == 0x08)
	{
		impl->channelMask = w.value;
		applyChannelMask();
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

	state.present = true;
	state.period = kOneShotPeriod;
	state.volume = 12;		// a trigger marker, not a measured level
	state.gate = impl->sounding[channel] && impl->now < impl->gateEnd[channel];

	return state;
}

const NoteTable& Rf5c68Decoder::noteTable (int channel) const
{
	(void) channel;
	return impl->table;
}

Family Rf5c68Decoder::family (int channel) const
{
	(void) channel;
	return Family::adpcmDrum;
}

} // namespace toybox::chips
