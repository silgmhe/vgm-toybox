#include "MikeyDecoder.h"

#include <cmath>
#include <cstdint>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	constexpr int kChannels = 4;

	/** The Lynx runs MIKEY from the 16 MHz system clock. */
	constexpr uint32_t kDefaultClock = 16000000;

	/** `2^(shift + 4)` master clocks per timer tick - mikey.c:355. */
	constexpr int kTicksShift = 4;

	double frequencyOf (uint32_t clock, int backup, int shift)
	{
		const double ticks = std::pow (2.0, (double) (shift + kTicksShift));
		const double divisor = 2.0 * (double) (backup + 1) * ticks;

		return (double) clock / divisor;
	}
}

struct MikeyDecoder::Impl
{
	struct Voice
	{
		int volume = 0;			// signed
		int backup = 0;			// the timer's reload value
		int shift = 0;			// bits 0-2 of CONTROL A
		bool integrate = false;	// feedback mode - a different timbre, not a different pitch
	};

	struct Table final : NoteTable
	{
		uint32_t clock = kDefaultClock;

		int noteFor (uint32_t period, int, int* finePitch) const override
		{
			const int backup = (int) (period & 0xFF);
			const int shift = (int) ((period >> 8) & 0x07);
			const double hz = frequencyOf (clock, backup, shift);

			if (hz <= 0.0 || hz > 4186.0)
			{
				// past MIDI's ceiling: a timer tick this fast is a trigger, not a dropped channel
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
	Voice voices[kChannels];
};

MikeyDecoder::MikeyDecoder() = default;
MikeyDecoder::~MikeyDecoder() = default;

void MikeyDecoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	impl = std::make_unique<Impl>();
	impl->table.clock = clock > 0 ? clock : kDefaultClock;
}

void MikeyDecoder::write (const RegisterWrite& write)
{
	if (impl == nullptr)
	{
		return;
	}

	// The four audio channels live at 0x20..0x3F, eight registers each; 0x40 and above are the
	// stereo attenuators, which shape the mix but carry no pitch.
	if (write.reg < 0x20 || write.reg >= 0x40)
	{
		return;
	}

	const int channel = (write.reg >> 3) & 0x03;

	Impl::Voice& voice = impl->voices[channel];

	switch (write.reg & 0x07)
	{
	case 0x0:		// volume in the low five bits; 0x20 selects the integrate mode
		voice.integrate = (write.value & 0x20) != 0;
		voice.volume = write.value & 0x1F;
		break;
	case 0x4:		// the reload value the counter borrows back to
		voice.backup = write.value;
		break;
	case 0x5:		// CONTROL A: bits 0-2 are the clock shift
		voice.shift = write.value & 0x07;
		break;
	default:
		break;		// 0x1 feedback, 0x2/0x3 status, 0x6 the live counter
	}
}

void MikeyDecoder::advance (uint32_t)
{
}

ChannelState MikeyDecoder::channelState (int channel) const
{
	ChannelState state;
	if (impl == nullptr || channel < 0 || channel >= kChannels)
	{
		return state;
	}

	const Impl::Voice& voice = impl->voices[channel];
	state.present = true;
	state.gate = voice.volume > 0 && voice.backup > 0;
	state.volume = state.gate ? (voice.volume > 15 ? 15 : voice.volume) : 0;
	state.period = (uint32_t) ((voice.shift << 8) | (voice.backup & 0xFF));

	if (! state.gate)
	{
		return state;
	}

	state.hz = frequencyOf (impl->table.clock, voice.backup, voice.shift);
	state.pitchSource = PitchSource::registerExact;
	return state;
}

const NoteTable& MikeyDecoder::noteTable (int) const
{
	static const Impl::Table fallback;
	return impl != nullptr ? impl->table : fallback;
}

ChannelKind MikeyDecoder::channelKind (int) const
{
	return ChannelKind::sn76489Square;		// a timer-driven square wave
}

} // namespace toybox::chips
