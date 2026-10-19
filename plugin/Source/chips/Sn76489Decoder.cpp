#include "Sn76489Decoder.h"

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	constexpr std::uint32_t kDefaultClock = 3579545;

	/** Three tone channels share the same relation: half a wave lasts 16 divider ticks, so the
		frequency is clock / (32 * period). */
	class SnToneTable final : public NoteTable
	{
	public:
		explicit SnToneTable (double clock) : clock (clock) {}

		int noteFor (std::uint32_t period, int, int* finePitch) const override
		{
			if (period == 0)
			{
				// the Sega parts the VGM format names stop instead of folding 0 to 0x400
				return -1;
			}

			return quantiseFrequency (clock / (32.0 * (double) period), finePitch);
		}

	private:
		double clock;
	};

	/** The noise generator's four modes are rates, not pitches, so each one keeps a GM drum key;
		the white-noise flag only changes the timbre. */
	class SnNoiseTable final : public NoteTable
	{
	public:
		int noteFor (std::uint32_t mode, int, int* finePitch) const override
		{
			if (finePitch != nullptr)
			{
				*finePitch = 0;
			}

			return kDrumBaseKey + (int) (mode & 0x03) * 2;
		}

	private:
		static constexpr int kDrumBaseKey = 38;		// GM acoustic snare, where the 2A03 starts
	};
}

struct Sn76489Decoder::Impl
{
	std::uint32_t period[3] = {};						// ten-bit dividers, low nibble first
	std::uint32_t pendingLow[3] = {};					// the first byte of an unfinished tone write
	std::uint8_t attenuation[numChannels] = { 15, 15, 15, 15 };
	std::uint8_t noiseMode = 0;							// 0xE0..0xE7: rate and white-noise flag
	int latch = 0;
	bool latchIsVolume = false;
	bool haveLow[3] = {};								// a low nibble is waiting for its high byte
	double clock = (double) kDefaultClock;
	std::unique_ptr<SnToneTable> toneTable;
	std::unique_ptr<SnNoiseTable> noiseTable;
};

Sn76489Decoder::Sn76489Decoder() : impl (std::make_unique<Impl>())
{
	reset (kDefaultClock, 44100.0);
}

Sn76489Decoder::~Sn76489Decoder() = default;

void Sn76489Decoder::reset (std::uint32_t clock, double sampleRate)
{
	(void) sampleRate;		// no timers: the latch and the registers are the whole state

	impl = std::make_unique<Impl>();

	impl->clock = (clock > 100000) ? (double) clock : (double) kDefaultClock;
	impl->toneTable = std::make_unique<SnToneTable> (impl->clock);
	impl->noiseTable = std::make_unique<SnNoiseTable>();
}

void Sn76489Decoder::write (const RegisterWrite& w)
{
	// the byte carries its own latch: there is no register index to compare against
	const std::uint8_t byte = w.reg;
	if ((byte & 0x80) != 0)
	{
		impl->latch = (byte >> 5) & 0x03;
		impl->latchIsVolume = (byte & 0x10) != 0;

		if (impl->latchIsVolume)
		{
			impl->attenuation[impl->latch] = byte & 0x0F;
			return;
		}

		if (impl->latch == noiseChannel)
		{
			impl->noiseMode = byte & 0x07;
			return;
		}

		impl->pendingLow[impl->latch] = byte & 0x0F;
		impl->haveLow[impl->latch] = true;
		return;
	}

	if (! impl->latchIsVolume && impl->latch < noiseChannel && impl->haveLow[impl->latch])
	{
		impl->period[impl->latch] = impl->pendingLow[impl->latch]
									| (((std::uint32_t) byte & 0x3F) << 4);
	}
}

void Sn76489Decoder::advance (std::uint32_t sampleTime)
{
	(void) sampleTime;		// no timers
}

ChannelState Sn76489Decoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= channelCount_)
	{
		return state;
	}

	const int attenuation = impl->attenuation[channel];

	state.present = true;
	state.volume = 15 - attenuation;
	state.gate = attenuation < 15;

	if (channel == noiseChannel)
	{
		state.period = impl->noiseMode;
		state.patch = 1;
		return state;
	}

	state.period = impl->period[channel];
	return state;
}

const NoteTable& Sn76489Decoder::noteTable (int channel) const
{
	if (channel == noiseChannel)
	{
		return *impl->noiseTable;
	}

	return *impl->toneTable;
}

Family Sn76489Decoder::family (int channel) const
{
	return channel == noiseChannel ? Family::sn76489Noise : Family::sn76489Square;
}

bool Sn76489Decoder::isDrum (int channel) const
{
	return channel == noiseChannel;
}

} // namespace toybox::chips
