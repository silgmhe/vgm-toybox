#include "Msm5232Decoder.h"

#include <cmath>
#include <cstdint>

namespace toybox::chips
{
namespace
{
	/** The ROM the register indexes has eighty-eight entries - the keyboard from A0 upwards. */
	constexpr int kFirstKey = 21;			// A0
	constexpr int kLastIndex = 0x57;		// the core clamps to this (msm5232.c:296)

	/** A value this high does not name a pitch: it puts the voice into noise mode (msm5232.c:290). */
	constexpr std::uint8_t kNoiseThreshold = 0xD8;
}

struct Msm5232Decoder::Impl
{
	struct Voice
	{
		int key = 0;			// index into the 88-entry ROM
		bool keyed = false;
		bool noise = false;
	};

	/** The index *is* the note, so the table has nothing to compute and nothing to measure. */
	struct Table final : NoteTable
	{
		int noteFor (std::uint32_t period, int, int* finePitch) const override
		{
			if (finePitch != nullptr)
			{
				*finePitch = 0;
			}

			const int index = (int) (period > (std::uint32_t) kLastIndex ? kLastIndex : period);
			return kFirstKey + index;
		}
	};

	Table table;
	Voice voices[8];
};

Msm5232Decoder::Msm5232Decoder() = default;
Msm5232Decoder::~Msm5232Decoder() = default;

void Msm5232Decoder::reset (std::uint32_t, double)
{
	impl = std::make_unique<Impl>();
}

void Msm5232Decoder::write (const RegisterWrite& write)
{
	if (impl == nullptr || write.reg >= 0x08)
	{
		return;
	}

	Impl::Voice& voice = impl->voices[write.reg & 0x07];

	// Bit 7 is the gate; the rest is the key index, unless the value is high enough to mean noise.
	voice.keyed = (write.value & 0x80) != 0;
	voice.noise = write.value >= kNoiseThreshold;
	if (! voice.noise)
	{
		voice.key = write.value & 0x7F;
		if (voice.key > kLastIndex)
		{
			voice.key = kLastIndex;
		}
	}
}

void Msm5232Decoder::advance (std::uint32_t)
{
	// the envelope is the chip's own business; every boundary here is a write
}

ChannelState Msm5232Decoder::channelState (int channel) const
{
	ChannelState state;
	if (impl == nullptr || channel < 0 || channel >= 8)
	{
		return state;
	}

	const Impl::Voice& voice = impl->voices[channel];
	state.present = true;
	state.gate = voice.keyed && (voice.noise || voice.key > 0);
	state.volume = state.gate ? 15 : 0;
	state.period = (std::uint32_t) voice.key;

	if (! state.gate)
	{
		return state;
	}

	if (voice.noise)
	{
		state.hz = 0.0;
		state.pitchSource = PitchSource::unpitched;
		return state;
	}

	// A4 is index 48 at 440 Hz, which is what makes "index + 21" a note number rather than a guess.
	state.hz = 440.0 * std::pow (2.0, (double) (kFirstKey + voice.key - 69) / 12.0);
	state.pitchSource = PitchSource::registerExact;
	return state;
}

const NoteTable& Msm5232Decoder::noteTable (int) const
{
	static const Impl::Table fallback;
	return impl != nullptr ? impl->table : fallback;
}

Family Msm5232Decoder::family (int channel) const
{
	return impl != nullptr && impl->voices[channel].noise ? Family::ayNoise : Family::sn76489Square;
}

} // namespace toybox::chips
