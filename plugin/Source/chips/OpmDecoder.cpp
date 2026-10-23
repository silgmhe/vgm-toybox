#include "OpmDecoder.h"

#include <cmath>
#include <cstdint>

namespace toybox::chips
{
namespace
{
	constexpr int kChannels = 8;

	/** KC << 6 | KF, so the 7-bit KC and the 6-bit KF fit in one period value. */
	class OpmNoteTable final : public NoteTable
	{
	public:
		void setClock (double chipClock)
		{
			clock = chipClock > 0.0 ? chipClock : 3579545.0;
		}

		int noteFor (std::uint32_t period, int, int* finePitch) const override
		{
			const int kc = (int) ((period >> 6) & 0x7F);
			const int kf = (int) (period & 0x3F);

			const int block = (kc >> 4) & 0x07;
			const int code = kc & 0x0F;

			// 13 puts code 0 at C#, and the code layout skips four values per octave.
			double semitone = 13.0 + 12.0 * (double) block
							  + (double) (code - (code >> 2)) + (double) kf / 64.0;

			// The output frequency follows the chip clock: an overclocked OPM plays sharp.
			semitone += 12.0 * std::log2 (clock / 3579545.0);

			const int nearest = (int) std::lround (semitone);
			*finePitch = (int) std::lround ((semitone - (double) nearest) * 100.0);
			return nearest;
		}

	private:
		double clock = 3579545.0;
	};
}

struct OpmDecoder::Impl
{
	struct Channel
	{
		int kc = 0;
		int kf = 0;
		bool havePitch = false;
		bool keyed = false;
	};

	Channel channels[kChannels];
	OpmNoteTable table;
};

OpmDecoder::OpmDecoder() : impl (std::make_unique<Impl>())
{
	impl->table.setClock (0.0);
}

OpmDecoder::~OpmDecoder() = default;

void OpmDecoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const std::uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	*impl = Impl();
	impl->table.setClock ((double) clock);
}

void OpmDecoder::write (const RegisterWrite& write)
{
	const std::uint8_t reg = write.reg;

	if (reg == 0x08)
	{
		// channel in bits 2..0, operator mask in bits 6..3 (M1 0x08, C1 0x10, M2 0x20, C2 0x40)
		const int channel = write.value & 0x07;
		impl->channels[channel].keyed = (write.value & 0x78) != 0;
		return;
	}

	if (reg >= 0x28 && reg < 0x30)
	{
		Impl::Channel& channel = impl->channels[reg & 0x07];
		channel.kc = write.value & 0x7F;
		channel.havePitch = true;
		return;
	}

	if (reg >= 0x30 && reg < 0x38)
	{
		impl->channels[reg & 0x07].kf = (write.value >> 2) & 0x3F;
	}
}

void OpmDecoder::advance (std::uint32_t)
{
	// OPM notes end on the key register, not on a timer.
}

int OpmDecoder::channelCount() const
{
	return kChannels;
}

ChannelState OpmDecoder::channelState (int channel) const
{
	ChannelState state;
	if (channel < 0 || channel >= kChannels)
	{
		return state;
	}

	const Impl::Channel& voice = impl->channels[channel];
	state.present = true;
	state.gate = voice.keyed && voice.havePitch;
	state.period = (std::uint32_t) ((voice.kc << 6) | voice.kf);
	state.volume = 15;
	return state;
}

const NoteTable& OpmDecoder::noteTable (int) const
{
	return impl->table;
}

ChannelKind OpmDecoder::channelKind (int) const
{
	return ChannelKind::fm;
}

} // namespace toybox::chips
