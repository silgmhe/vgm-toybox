#include "WswanDecoder.h"

#include <algorithm>
#include <cmath>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	constexpr std::uint32_t kDefaultClock = 3072000;	// the WonderSwan's 3.072 MHz crystal

	// The period field is 11 bits wide, so the two cases without a divider - the noise generator
	// and the voice stream - are flagged in the bits above it, as on the HuC6280.
	constexpr std::uint32_t kNoiseFlag = 0x1000;
	constexpr std::uint32_t kVoiceFlag = 0x2000;

	constexpr int kVoiceNote = 60;					// trigger marker, not the sample's pitch
	constexpr std::uint32_t kVoiceLength = 4410;	// 100 ms at 44.1 kHz

	/** Wave: the core steps the 32-entry waveform once every 32 * (2048 - frequency) clocks, so
		one cycle takes 32 steps and frequency 0 is the lowest pitch rather than the highest.

		Noise: the LFSR is clocked at that same step rate, which gives the generator no pitch of
		its own - the rate picks a GM drum key instead. The divider spans exactly eleven octaves
		(2048 down to 1), more than the drum key range, so it is compressed onto twenty keys. */
	class WswanTable final : public NoteTable
	{
	public:
		explicit WswanTable (double clock) : clock (clock) {}

		int noteFor (std::uint32_t period, int, int* finePitch) const override
		{
			if ((period & kVoiceFlag) != 0)
			{
				if (finePitch != nullptr)
				{
					*finePitch = 0;
				}

				return kVoiceNote;
			}

			if ((period & kNoiseFlag) != 0)
			{
				return noiseNote (period & 0x07FF, finePitch);
			}

			return quantiseFrequency (toneHz (period & 0x07FF), finePitch);
		}

	private:
		double toneHz (std::uint32_t frequency) const
		{
			const double divider = 2048.0 - (double) frequency;
			return clock / (32.0 * std::max (divider, 1.0));
		}

		int noiseNote (std::uint32_t frequency, int* finePitch) const
		{
			if (finePitch != nullptr)
			{
				*finePitch = 0;
			}

			const double divider = 2048.0 - (double) frequency;
			const double position = std::log2 (2048.0 / std::max (divider, 1.0)) / 11.0;
			const long level = std::lround (std::clamp (position, 0.0, 1.0) * 20.0);

			return kDrumBaseKey + (int) level;
		}

		static constexpr int kDrumBaseKey = 38;		// GM acoustic snare, where the 2A03 starts

		double clock;
	};
}

struct WswanDecoder::Impl
{
	struct Channel
	{
		std::uint16_t frequency = 0x07FF;	// the chip powers up at 0x07FF, which is not silence
		std::uint8_t left = 0;
		std::uint8_t right = 0;
	};

	Channel channels[4];
	std::uint8_t mode = 0;				// 0x90: per-channel enable, voice, sweep, noise
	std::uint8_t pcmVolume = 0;			// 0x94
	std::int8_t sweepStep = 0;			// 0x8C, signed: it counts up or down
	std::uint32_t sweepInterval = 1;	// 0x8D, in samples
	std::uint16_t sweepPeriod = 0;		// where the sweep unit currently is
	std::uint32_t sweepNext = 0;
	std::uint32_t voiceEnd = 0;
	std::uint32_t now = 0;
	double clock = (double) kDefaultClock;
	double sampleRate = 44100.0;
	std::unique_ptr<WswanTable> table;

	bool sweepRunning (void) const { return sweepStep != 0 && (mode & 0x40) != 0; }
	bool voiceRunning (void) const { return (mode & 0x20) != 0; }
	bool noiseRunning (void) const { return (mode & 0x80) != 0; }
};

WswanDecoder::WswanDecoder() : impl (std::make_unique<Impl>())
{
	reset (ChipConfig { kDefaultClock, 44100.0 });
}

WswanDecoder::~WswanDecoder() = default;

void WswanDecoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const std::uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	impl = std::make_unique<Impl>();

	// a missing clock in the header is common: the WonderSwan clock is the CPU crystal
	impl->clock = (clock > 100000) ? (double) clock : (double) kDefaultClock;
	impl->sampleRate = (sampleRate > 0.0) ? sampleRate : 44100.0;
	impl->table = std::make_unique<WswanTable> (impl->clock);

	setSweepTime (0x1F, 0);		// the value the chip powers up with
}

void WswanDecoder::setSweepTime (std::uint8_t value, std::uint32_t sampleTime)
{
	// one tick every (n + 1) sweep units, a unit being 32 HBlanks of 256 clocks each
	const double hblanks = (double) (((std::uint32_t) value + 1u) * 32u + 1u);
	const double samples = hblanks * 256.0 * impl->sampleRate / impl->clock;

	impl->sweepInterval = std::max<std::uint32_t> (1u, (std::uint32_t) std::lround (samples));
	impl->sweepNext = sampleTime + impl->sweepInterval;
}

void WswanDecoder::write (const RegisterWrite& w)
{
	Impl::Channel& channel0 = impl->channels[WswanDecoder::channel0];
	Impl::Channel& channel1 = impl->channels[WswanDecoder::channel1];
	Impl::Channel& channel2 = impl->channels[WswanDecoder::channel2];
	Impl::Channel& channel3 = impl->channels[WswanDecoder::channel3];

	switch (w.reg)
	{
	// The WonderSwan sound registers, by address - the numbers alone say nothing:
	//
	//   0x80/0x81 … 0x86/0x87  channel 0-3 frequency, low byte then high byte; channel 2 shares
	//                          its registers with the sweep unit, which is why 0x84 latches the
	//                          period as well
	//   0x88..0x8B             channel 0-3 volume (4 bit)
	//   0x89                   doubles as the PCM data port while voice mode streams, hence the
	//                          separate case below
	//   0x8C                   sweep step, signed
	//   0x8D                   sweep time
	//   0x8E                   noise LFSR tap - the noise's colour, not its pitch
	//   0x8F                   waveform base address - the shape, not the pitch
	//   0x90                   control: per-channel enable, channel 2 sweep, voice mode
	//   0x94                   PCM volume for the voice stream
	case 0x80:
		channel0.frequency = (std::uint16_t) ((channel0.frequency & 0xFF00) | w.value);
		break;
	case 0x81:
		channel0.frequency = (std::uint16_t) ((channel0.frequency & 0x00FF)
											 | ((std::uint16_t) w.value << 8));
		break;
	case 0x82:
		channel1.frequency = (std::uint16_t) ((channel1.frequency & 0xFF00) | w.value);
		break;
	case 0x83:
		channel1.frequency = (std::uint16_t) ((channel1.frequency & 0x00FF)
											 | ((std::uint16_t) w.value << 8));
		break;
	case 0x84:
		channel2.frequency = (std::uint16_t) ((channel2.frequency & 0xFF00) | w.value);
		impl->sweepPeriod = channel2.frequency;
		break;
	case 0x85:
		channel2.frequency = (std::uint16_t) ((channel2.frequency & 0x00FF)
											 | ((std::uint16_t) w.value << 8));
		impl->sweepPeriod = channel2.frequency;
		break;
	case 0x86:
		channel3.frequency = (std::uint16_t) ((channel3.frequency & 0xFF00) | w.value);
		break;
	case 0x87:
		channel3.frequency = (std::uint16_t) ((channel3.frequency & 0x00FF)
											 | ((std::uint16_t) w.value << 8));
		break;
	case 0x88:
	case 0x8A:
	case 0x8B:
	{
		Impl::Channel& current = impl->channels[w.reg - 0x88];
		current.left = (std::uint8_t) ((w.value >> 4) & 0x0F);
		current.right = (std::uint8_t) (w.value & 0x0F);
		break;
	}
	case 0x89:
		// the PCM sample byte, not a volume: it only matters while voice mode streams it
		if (impl->voiceRunning() && w.sampleTime >= impl->voiceEnd)
		{
			// a stream of DMA bytes: rate-limit the trigger so the channel reads as drum hits
			// instead of one note per byte
			impl->voiceEnd = w.sampleTime + kVoiceLength;
		}
		break;
	case 0x8C:
		impl->sweepStep = (std::int8_t) w.value;
		break;
	case 0x8D:
		setSweepTime (w.value, w.sampleTime);
		break;
	case 0x8E:
		break;		// LFSR tap: a different noise colour, the same rate
	case 0x8F:
		break;		// waveform base address: the shape does not change the note
	case 0x90:
	{
		const std::uint8_t previous = impl->mode;
		impl->mode = w.value;

		if ((previous & 0x20) == 0 && (w.value & 0x20) != 0)
		{
			impl->voiceEnd = w.sampleTime + kVoiceLength;
		}

		if ((previous & 0x40) == 0 && (w.value & 0x40) != 0)
		{
			impl->sweepNext = w.sampleTime + impl->sweepInterval;
		}
		break;
	}
	case 0x94:
		impl->pcmVolume = w.value;
		break;
	default:
		break;		// 0x91-0x93: master volume and the LFSR read-back, nothing to hear
	}
}

void WswanDecoder::stepSweep (std::uint32_t sampleTime)
{
	if (! impl->sweepRunning() || sampleTime < impl->sweepNext)
	{
		return;
	}

	// every step that falls into the elapsed span, applied at once: between two polls only the
	// resulting pitch can be observed
	const std::uint32_t steps = (sampleTime - impl->sweepNext) / impl->sweepInterval + 1u;
	const int value = (int) impl->sweepPeriod + (int) impl->sweepStep * (int) steps;

	impl->sweepPeriod = (std::uint16_t) (value & 0x07FF);
	impl->sweepNext += steps * impl->sweepInterval;
}

void WswanDecoder::advance (std::uint32_t sampleTime)
{
	impl->now = sampleTime;
	stepSweep (sampleTime);
}

ChannelState WswanDecoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= channelCount_)
	{
		return state;
	}

	const Impl::Channel& current = impl->channels[channel];
	const int volume = std::max<int> (current.left, current.right);
	const bool enabled = (impl->mode & (1 << channel)) != 0;

	state.present = true;
	state.patch = 0;
	state.volume = volume;

	if (channel == WswanDecoder::channel1 && impl->voiceRunning())
	{
		// the core plays the DMA byte through the PCM volume, not the waveform: no pitch, and
		// the hit is over when the buffer's worth of bytes has been sent
		const int pcmVolume = (impl->pcmVolume & 0x0C) * 2;		// the core's 0..24
		state.gate = impl->now < impl->voiceEnd && pcmVolume > 0;
		state.period = kVoiceFlag;
		state.volume = pcmVolume * 15 / 24;
		return state;
	}

	if (channel == WswanDecoder::channel3 && impl->noiseRunning())
	{
		// 0xFFFF stops the LFSR on channel 3 the same way it stops the waveform on the others
		state.gate = enabled && volume > 0 && current.frequency != 0xFFFF;
		state.period = kNoiseFlag | (current.frequency & 0x07FF);
		return state;
	}

	// 0xFFFF is the one value the core treats as silence on a tone channel
	const bool silent = current.frequency == 0xFFFF && ! (channel == WswanDecoder::channel2
														  && impl->sweepRunning());
	const std::uint32_t period = (channel == WswanDecoder::channel2 && impl->sweepRunning())
									 ? (std::uint32_t) (impl->sweepPeriod & 0x07FF)
									 : (std::uint32_t) (current.frequency & 0x07FF);

	state.gate = enabled && volume > 0 && ! silent;
	state.period = period;

	return state;
}

const NoteTable& WswanDecoder::noteTable (int channel) const
{
	(void) channel;		// one table: the tone, noise and voice cases differ only in the period
	return *impl->table;
}

Family WswanDecoder::family (int channel) const
{
	if (channel == WswanDecoder::channel1 && impl->voiceRunning())
	{
		return Family::adpcmDrum;
	}

	if (channel == WswanDecoder::channel3 && impl->noiseRunning())
	{
		return Family::wsNoise;
	}

	return Family::wsWave;
}

bool WswanDecoder::isDrum (int channel) const
{
	return channel == WswanDecoder::channel3 && impl->noiseRunning();
}

} // namespace toybox::chips
