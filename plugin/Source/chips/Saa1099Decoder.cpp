#include "Saa1099Decoder.h"

#include <algorithm>
#include <cmath>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	constexpr std::uint32_t kDefaultClock = 8000000;

	// A channel that plays noise instead of its divider has no pitch at all; the noise generator's
	// rate travels in this bit, above the 8-bit frequency register.
	constexpr std::uint32_t kNoiseFlag = 0x100;

	/** The core works at clock / 256 and toggles a channel every (511 - frequency) / 2^octave of
		those samples, which is half a wave: the output is clock * 2^octave / (512 * (511 - f)).

		Noise: the generator's four rates are three fixed divisions of the clock and, in the fourth
		case, the channel's own tone divider; only their order survives as a drum key. */
	class Saa1099Table final : public NoteTable
	{
	public:
		explicit Saa1099Table (double clock) : clock (clock) {}

		int noteFor (std::uint32_t period, int block, int* finePitch) const override
		{
			if ((period & kNoiseFlag) != 0)
			{
				if (finePitch != nullptr)
				{
					*finePitch = 0;
				}

				return kDrumBaseKey + (int) (period & 0x03) * 2;
			}

			if (period > 255)
			{
				return -1;
			}

			const double octave = std::exp2 ((double) std::clamp (block, 0, 7));
			const double divisor = 512.0 * (511.0 - (double) period);

			return quantiseFrequency (clock * octave / divisor, finePitch);
		}

	private:
		static constexpr int kDrumBaseKey = 38;		// GM acoustic snare, where the 2A03 starts

		double clock;
	};
}

struct Saa1099Decoder::Impl
{
	std::uint8_t amplitude[numChannels] = {};	// registers 0x00-0x05
	std::uint8_t frequency[numChannels] = {};	// registers 0x08-0x0D
	int octave[numChannels] = {};				// registers 0x10-0x12, one value per pair
	bool toneEnabled[numChannels] = {};			// register 0x14
	bool noiseEnabled[numChannels] = {};		// register 0x15
	std::uint8_t noiseParams[2] = {};			// register 0x16, one value per noise generator
	bool allChannels = false;					// register 0x1C, bit 0
	double clock = (double) kDefaultClock;
	std::unique_ptr<Saa1099Table> table;
};

Saa1099Decoder::Saa1099Decoder() : impl (std::make_unique<Impl>())
{
	reset (ChipConfig { kDefaultClock, 44100.0 });
}

Saa1099Decoder::~Saa1099Decoder() = default;

void Saa1099Decoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const std::uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	(void) sampleRate;		// the chip has no timers; the registers are the whole state

	impl = std::make_unique<Impl>();

	impl->clock = (clock > 100000) ? (double) clock : (double) kDefaultClock;
	impl->table = std::make_unique<Saa1099Table> (impl->clock);
}

void Saa1099Decoder::write (const RegisterWrite& w)
{
	const int channel = w.reg & 0x07;

	switch (w.reg)
	{
	// The SAA1099 register map, by address (six tone channels in two groups of three):
	//
	//   0x00..0x05  amplitude, one byte per channel; the left and right nibbles are separate
	//               outputs, and the louder nibble is the volume this decoder reports
	//   0x08..0x0D  frequency, one byte per channel - the divider that gives the note
	//   0x10..0x12  envelope control, one byte per *pair* of channels
	//   0x14/0x15   per-channel enable and the envelope/sync side of the same block
	//   0x16        the two noise generators' parameters, low nibble = generator 0
	//   0x18/0x19   envelope waveform and resolution
	//   0x1C        reset / sync
	case 0x00:
	case 0x01:
	case 0x02:
	case 0x03:
	case 0x04:
	case 0x05:
		// left and right nibbles are separate outputs; the louder one is the channel's volume
		impl->amplitude[channel] = (std::uint8_t) std::max (w.value & 0x0F, (w.value >> 4) & 0x0F);
		break;
	case 0x08:
	case 0x09:
	case 0x0A:
	case 0x0B:
	case 0x0C:
	case 0x0D:
		impl->frequency[channel] = w.value;
		break;
	case 0x10:
	case 0x11:
	case 0x12:
	{
		const int first = (w.reg - 0x10) * 2;
		impl->octave[first] = w.value & 0x07;
		impl->octave[first + 1] = (w.value >> 4) & 0x07;
		break;
	}
	case 0x14:
		for (int i = 0; i < numChannels; ++i)
		{
			impl->toneEnabled[i] = (w.value & (1 << i)) != 0;
		}
		break;
	case 0x15:
		for (int i = 0; i < numChannels; ++i)
		{
			impl->noiseEnabled[i] = (w.value & (1 << i)) != 0;
		}
		break;
	case 0x16:
		impl->noiseParams[0] = w.value & 0x03;
		impl->noiseParams[1] = (w.value >> 4) & 0x03;
		break;
	case 0x1C:
		impl->allChannels = (w.value & 0x01) != 0;
		break;
	default:
		break;		// noise parameters, envelopes, reset: nothing a note needs
	}
}

void Saa1099Decoder::advance (std::uint32_t sampleTime)
{
	(void) sampleTime;		// no timers
}

ChannelState Saa1099Decoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= channelCount_)
	{
		return state;
	}

	const bool noise = impl->noiseEnabled[channel];
	const bool noiseOnly = noise && ! impl->toneEnabled[channel];

	state.present = true;
	state.block = impl->octave[channel];
	state.volume = impl->amplitude[channel];
	state.patch = noise ? 1 : 0;

	// the divider only carries a pitch while the tone generator runs; a channel that plays nothing
	// but noise takes its drum key from the noise generator's rate instead
	state.period = noiseOnly ? (kNoiseFlag | impl->noiseParams[channel / 3]) : impl->frequency[channel];

	state.gate = impl->allChannels && state.volume > 0
				 && (impl->toneEnabled[channel] || noise);

	return state;
}

bool Saa1099Decoder::isDrum (int channel) const
{
	if (channel < 0 || channel >= channelCount_)
	{
		return false;
	}

	return impl->noiseEnabled[channel] && ! impl->toneEnabled[channel];
}

const NoteTable& Saa1099Decoder::noteTable (int channel) const
{
	(void) channel;		// the octave travels in ChannelState::block, so one table serves all six
	return *impl->table;
}

ChannelKind Saa1099Decoder::channelKind (int channel) const
{
	(void) channel;
	return ChannelKind::saaSquare;
}

} // namespace toybox::chips
