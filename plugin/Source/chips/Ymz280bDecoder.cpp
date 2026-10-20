#include "Ymz280bDecoder.h"

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	constexpr int kOneShotNote = 60;
	constexpr std::uint32_t kOneShotPeriod = 0x80000000u;

	// 80 ms at 44.1 kHz, the same marker the OPN's ADPCM-A and the RF5C68 use.
	constexpr std::uint32_t kOneShotGate = 3528;

	class Ymz280bNoteTable final : public NoteTable
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

struct Ymz280bDecoder::Impl
{
	struct Voice
	{
		bool keyOn = false;
		std::uint8_t level = 0;
		std::uint16_t fnum = 0;		// the playback rate register; kept for the M4 rate mapping
		std::uint32_t gateEnd = 0;
		bool gateOpen = false;
	};

	Voice voices[numChannels];
	std::uint32_t now = 0;
	Ymz280bNoteTable table;

	void apply (std::uint8_t reg, std::uint8_t value, std::uint32_t time)
	{
		const int voiceIndex = (reg >> 2) & 7;
		Voice& voice = voices[voiceIndex];

		switch (reg & 0xE3)
		{
		case 0x00:
			voice.fnum = (std::uint16_t) ((voice.fnum & 0x100) | value);
			break;
		case 0x01:
		{
			voice.fnum = (std::uint16_t) ((voice.fnum & 0xFF) | ((value & 0x01) << 8));

			const bool wasOn = voice.keyOn;
			voice.keyOn = (value & 0x80) != 0;
			if (voice.keyOn && ! wasOn)
			{
				voice.gateEnd = time + kOneShotGate;
				voice.gateOpen = true;
			}
			else if (! voice.keyOn)
			{
				voice.gateOpen = false;
			}

			break;
		}
		case 0x02:
			voice.level = value;
			break;
		default:
			break;		// pan, loop and start/end addresses say nothing about the note
		}
	}
};

Ymz280bDecoder::Ymz280bDecoder() : impl (std::make_unique<Impl>())
{
	reset (16934400, 44100.0);
}

Ymz280bDecoder::~Ymz280bDecoder() = default;

void Ymz280bDecoder::reset (std::uint32_t clock, double sampleRate)
{
	(void) clock;			// the trigger does not depend on the clock; M4's rate mapping will
	(void) sampleRate;
	impl = std::make_unique<Impl>();
}

void Ymz280bDecoder::write (const RegisterWrite& w)
{
	// VGM's 0x5D carries the register number in `aa` and the value in `dd` - measured, not assumed:
	// a real rip writes 0x5D 0x0D 0x20, 0x5D 0x6C 0xBD and so on, never the 0x00/0x01 pair the
	// hardware's byte-wide latch would need. The core does the latching internally; here the
	// register number is already resolved.
	impl->apply ((std::uint8_t) w.reg, w.value, w.sampleTime);
}

void Ymz280bDecoder::advance (std::uint32_t sampleTime)
{
	impl->now = sampleTime;

	for (auto& voice : impl->voices)
	{
		if (voice.gateOpen && sampleTime >= voice.gateEnd)
		{
			voice.gateOpen = false;
		}
	}
}

ChannelState Ymz280bDecoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= channelCount_)
	{
		return state;
	}

	const Impl::Voice& voice = impl->voices[channel];
	state.present = true;
	state.gate = voice.gateOpen && voice.level > 0;		// a silenced voice is not a note
	state.period = kOneShotPeriod;
	state.volume = (int) voice.level * 15 / 255;
	return state;
}

const NoteTable& Ymz280bDecoder::noteTable (int channel) const
{
	(void) channel;
	return impl->table;
}

Family Ymz280bDecoder::family (int channel) const
{
	(void) channel;
	return Family::adpcmDrum;
}

} // namespace toybox::chips
