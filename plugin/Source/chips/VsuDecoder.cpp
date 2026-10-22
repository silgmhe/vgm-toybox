#include "VsuDecoder.h"

#include <cstdint>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	/** Thirty-two steps make one cycle of the wave RAM. */
	constexpr double kStepsPerCycle = 32.0;

	/** `2048 - freq` is the reload value, so the range of the divider is 1..2048. */
	constexpr double kDividerRange = 2048.0;

	/** The clock to assume when the VGM header carries none for this chip. */
	constexpr std::uint32_t kDefaultClock = 5370000;

	/** The sixth voice is an LFSR noise source, not a wavetable voice. */
	constexpr int kNoiseChannel = 5;

	double frequencyOf (std::uint32_t clock, std::uint16_t freq)
	{
		const double remaining = kDividerRange - (double) (freq & 0x7FF);
		if (remaining <= 0.0)
		{
			return 0.0;
		}

		return (double) clock / (kStepsPerCycle * remaining);
	}
}

struct VsuDecoder::Impl
{
	struct Voice
	{
		std::uint16_t freq = 0;			// eleven bits: low byte at 0x2, top three at 0x3
		bool keyed = false;
		int waveform = 0;
		int envelope = 0;
	};

	/** All six voices share one formula, so one table serves them. */
	struct Table final : NoteTable
	{
		std::uint32_t clock = kDefaultClock;

		int noteFor (std::uint32_t period, int, int* finePitch) const override
		{
			const double hz = frequencyOf (clock, (std::uint16_t) period);
			if (hz <= 0.0 || hz > 4186.0)
			{
				// past MIDI's ceiling - a noise-rate divider, reported as a trigger rather than dropped
				if (finePitch != nullptr)
				{
					*finePitch = 0;
				}

				return 60;
			}

			return quantiseFrequency (hz, finePitch);
		}
	};

	Table table;
	Voice voices[6];
};

VsuDecoder::VsuDecoder() = default;
VsuDecoder::~VsuDecoder() = default;

void VsuDecoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const std::uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	(void) sampleRate;
	impl = std::make_unique<Impl>();
	impl->table.clock = clock > 0 ? clock : kDefaultClock;
}

void VsuDecoder::write (const RegisterWrite& write)
{
	if (impl == nullptr)
	{
		return;
	}

	// The command's offset is a byte address: one voice owns sixteen of them.
	const int voice = (write.reg >> 4) & 0x0F;
	if (voice > 5)
	{
		return;
	}

	const int reg = write.reg & 0x0F;
	Impl::Voice& state = impl->voices[voice];

	switch (reg)
	{
	case 0x0:		// IntlControl: bit 7 keys the voice on; the low five bits are the interval
		state.keyed = (write.value & 0x80) != 0;
		break;
	case 0x2:		// frequency, low eight bits
		state.freq = (std::uint16_t) ((state.freq & 0x700) | write.value);
		break;
	case 0x3:		// frequency, high three bits
		state.freq = (std::uint16_t) ((state.freq & 0x0FF) | ((write.value & 0x07) << 8));
		break;
	case 0x4:		// envelope control - level, not pitch
		state.envelope = write.value;
		break;
	case 0x5:		// waveform base address - the shape, not the note
		state.waveform = write.value;
		break;
	default:
		break;		// 0x1, 0x6..0xF: sweep, modulation and the remaining envelope bits
	}
}

void VsuDecoder::advance (std::uint32_t)
{
	// every boundary comes from a write; there is no envelope to step
}

ChannelState VsuDecoder::channelState (int channel) const
{
	ChannelState state;
	if (impl == nullptr || channel < 0 || channel >= 6)
	{
		return state;
	}

	const Impl::Voice& voice = impl->voices[channel];
	state.present = true;
	state.gate = voice.keyed && voice.freq > 0;
	state.volume = state.gate ? 15 : 0;
	state.patch = voice.waveform;
	state.period = voice.freq;

	if (! state.gate)
	{
		return state;
	}

	if (channel == kNoiseChannel)
	{
		// an LFSR source: it sounds, but what it produces has no period to name
		state.hz = 0.0;
		state.pitchSource = PitchSource::unpitched;
		return state;
	}

	state.hz = frequencyOf (impl->table.clock, voice.freq);
	state.pitchSource = PitchSource::registerExact;
	return state;
}

const NoteTable& VsuDecoder::noteTable (int) const
{
	static const Impl::Table fallback;
	return impl != nullptr ? impl->table : fallback;
}

Family VsuDecoder::family (int channel) const
{
	return channel == kNoiseChannel ? Family::ayNoise : Family::wavetableWave;
}

} // namespace toybox::chips
