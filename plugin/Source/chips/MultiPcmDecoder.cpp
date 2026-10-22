#include "MultiPcmDecoder.h"

#include <cmath>
#include <cstdint>

namespace toybox::chips
{
namespace
{
	/** Unity ratio - pitch 0, octave 0 - is placed at C4, the same convention as the other chips that
		can only speak in ratios. The absolute octave needs the sample's rate from the ROM metadata. */
	constexpr int kUnityNote = 60;

	constexpr int kVoices = 28;

	double ratioOf (int octave, int pitch)
	{
		const double linear = (1024.0 + (double) pitch) / 1024.0;
		const double octaveScale = std::pow (2.0, (double) octave);

		return linear * octaveScale;
	}
}

struct MultiPcmDecoder::Impl
{
	struct Voice
	{
		std::uint16_t pitch = 0;			// ten bits
		int octave = 0;						// four bits, signed
		bool keyed = false;
		int attenuation = 0x7F;
		int sampleIndex = 0;
	};

	struct Table final : NoteTable
	{
		int noteFor (std::uint32_t period, int, int* finePitch) const override
		{
			const int octave = (int) ((period >> 10) & 0xFF);
			const int pitch = (int) (period & 0x3FF);
			const double ratio = ratioOf ((std::int8_t) (std::uint8_t) octave, pitch);

			if (ratio <= 0.0)
			{
				if (finePitch != nullptr)
				{
					*finePitch = 0;
				}

				return kUnityNote;
			}

			const double semitones = 12.0 * std::log2 (ratio);
			if (semitones < -48.0 || semitones > 48.0)
			{
				// outside anything a MIDI note can say; report the trigger rather than dropping it
				if (finePitch != nullptr)
				{
					*finePitch = 0;
				}

				return kUnityNote;
			}

			const double exact = (double) kUnityNote + semitones;
			const int nearest = (int) std::floor (exact + 0.5);
			if (finePitch != nullptr)
			{
				*finePitch = (int) std::lround ((exact - (double) nearest) * 100.0);
			}

			return nearest;
		}
	};

	Table table;
	Voice voices[kVoices];

	/** The chip is programmed through three ports, not by addressing a voice directly (multipcm.c:1-30):
		offset 1 selects the voice, offset 2 sets which of its eight registers the next data write
		lands in, and offset 0 writes that register. Every write in the rips goes through this door. */
	int currentSlot = -1;
	int address = 0;
};

MultiPcmDecoder::MultiPcmDecoder() = default;
MultiPcmDecoder::~MultiPcmDecoder() = default;

void MultiPcmDecoder::reset (std::uint32_t, double)
{
	impl = std::make_unique<Impl>();
}

void MultiPcmDecoder::write (const RegisterWrite& write)
{
	if (impl == nullptr)
	{
		return;
	}

	// Three ports, in the core's own terms: 1 selects the voice, 2 the register, 0 the data.
	if (write.reg == 0x01)
	{
		impl->currentSlot = write.value & 0x1F;
		return;
	}

	if (write.reg == 0x02)
	{
		impl->address = write.value > 7 ? 7 : write.value;
		return;
	}

	if (write.reg != 0x00 || impl->currentSlot < 0 || impl->currentSlot >= kVoices)
	{
		return;		// 0x10..0x12 are the Sega banking registers, not a voice
	}

	const int voice = impl->currentSlot;
	Impl::Voice& state = impl->voices[voice];

	switch (impl->address)
	{
	case 0x1:		// sample index into the ROM metadata table
		state.sampleIndex = write.value;
		break;
	case 0x2:		// pitch, low eight bits
		state.pitch = (std::uint16_t) ((state.pitch & 0x300) | write.value);
		break;
	case 0x3:		// four-bit signed octave above a ten-bit pitch
		state.octave = (std::int8_t) (std::uint8_t) (write.value >> 4);
		state.pitch = (std::uint16_t) ((state.pitch & 0x0FF) | ((write.value & 0x03) << 8));
		break;
	case 0x4:		// bit 7 keys the voice on; clearing it releases it
		state.keyed = (write.value & 0x80) != 0;
		break;
	case 0x5:		// bits 1-7 are attenuation, 0 loudest
		state.attenuation = (write.value >> 1) & 0x7F;
		break;
	default:
		break;		// 0x0 pan, 0x6/0x7 the LFOs - none of them a pitch
	}
}

void MultiPcmDecoder::advance (std::uint32_t)
{
}

ChannelState MultiPcmDecoder::channelState (int channel) const
{
	ChannelState state;
	if (impl == nullptr || channel < 0 || channel >= kVoices)
	{
		return state;
	}

	const Impl::Voice& voice = impl->voices[channel];
	state.present = true;
	state.gate = voice.keyed;
	state.volume = state.gate ? 15 - (voice.attenuation * 15 / 0x7F) : 0;
	state.period = (std::uint32_t) (((voice.octave & 0xFF) << 10) | (voice.pitch & 0x3FF));
	state.patch = voice.sampleIndex;

	if (! state.gate)
	{
		return state;
	}

	// A ratio, not a frequency: the sample's own rate would be needed for Hz, and it lives in the ROM.
	state.hz = 0.0;
	state.pitchSource = PitchSource::registerExact;
	return state;
}

const NoteTable& MultiPcmDecoder::noteTable (int) const
{
	static const Impl::Table fallback;
	return impl != nullptr ? impl->table : fallback;
}

Family MultiPcmDecoder::family (int) const
{
	return Family::c140Pcm;		// a sampled voice with a rate the registers scale
}

} // namespace toybox::chips
