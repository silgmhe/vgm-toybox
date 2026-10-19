#include "Ym2612Decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace toybox::chips
{
namespace
{
	int quantise (double hz, int* finePitch)
	{
		if (finePitch != nullptr)
		{
			*finePitch = 0;
		}

		if (hz <= 0.0)
		{
			return -1;
		}

		const double exact = 69.0 + 12.0 * std::log2 (hz / 440.0);
		const int rounded = (int) std::lround (exact);
		if (rounded < 0 || rounded > 127)
		{
			return -1;
		}

		if (finePitch != nullptr)
		{
			*finePitch = (std::int16_t) std::lround ((exact - (double) rounded) * 100.0);
		}

		return rounded;
	}

	/** One bit per operator that reaches the output, in register order (0x30, 0x34, 0x38, 0x3C).
		Taken from the core's own per-algorithm early-out: algorithms 0-3 have a single carrier,
		4 has two, 5 and 6 have three, and 7 sums all four. */
	constexpr std::uint8_t kCarriers[8] = { 0x08, 0x08, 0x08, 0x08,
											0x0C, 0x0E, 0x0E, 0x0F };

	/** An 11-bit f-number and a 3-bit block address a note through the OPN frequency relation.
		The table is clock-dependent: the same register value plays a different pitch on an
		overclocked chip, so the VGM header's clock is part of the mapping. */
	class OpnNoteTable final : public NoteTable
	{
	public:
		void setClock (double chipClock)
		{
			clock = chipClock > 0.0 ? chipClock : 7670453.0;
		}

		int noteFor (std::uint32_t fnum, int block, int* finePitch) const override
		{
			const int octave = std::clamp (block, 0, 7);
			const double hz = (double) fnum * clock / (144.0 * std::exp2 (20.0 - (double) octave));
			return quantise (hz, finePitch);
		}

	private:
		double clock = 7670453.0;
	};
}

struct Ym2612Decoder::Impl
{
	struct Channel
	{
		int fnum = 0;
		int block = 0;
		int algorithm = 0;
		bool keyed = false;
		std::uint8_t totalLevel[4] = { 0x7F, 0x7F, 0x7F, 0x7F };
	};

	Channel channels[numChannels];
	OpnNoteTable table;
	bool dacEnabled = false;

	void keyEvent (std::uint8_t data)
	{
		const int channel = (int) (data & 0x03) + (((data & 0x04) != 0) ? 3 : 0);
		if (channel >= numChannels)
		{
			return;
		}

		// Bits 4-7 are the operator mask; a zero mask is the key-off. A partial mask that does
		// not include a carrier is silent, which channelState() deals with.
		channels[channel].keyed = (data & 0xF0) != 0;
	}

	int loudestCarrier (const Channel& channel) const
	{
		const std::uint8_t carriers = kCarriers[channel.algorithm & 0x07];
		int level = 0x7F;
		for (int op = 0; op < 4; ++op)
		{
			if ((carriers & (1 << op)) != 0)
			{
				level = std::min (level, (int) channel.totalLevel[op]);
			}
		}

		return level;
	}
};

Ym2612Decoder::Ym2612Decoder() : impl (std::make_unique<Impl>())
{
	impl->table.setClock (0.0);
}

Ym2612Decoder::~Ym2612Decoder() = default;

void Ym2612Decoder::reset (std::uint32_t clock, double)
{
	*impl = Impl();
	impl->table.setClock ((double) clock);
}

void Ym2612Decoder::write (const RegisterWrite& write)
{
	const std::uint8_t reg = write.reg;
	const int port = write.port != 0 ? 1 : 0;

	// Key on/off is the only register that addresses a channel across both ports.
	if (reg == 0x28 && port == 0)
	{
		impl->keyEvent (write.value);
		return;
	}

	if (reg == 0x2B && port == 0)
	{
		impl->dacEnabled = (write.value & 0x80) != 0;
		return;
	}

	const int channel = (int) (reg & 0x03) + port * 3;
	if (channel >= numChannels)
	{
		return;
	}

	Impl::Channel& state = impl->channels[channel];

	switch (reg & 0xF0)
	{
	case 0x40:		// total level, one per operator: the only volume an FM channel has
		state.totalLevel[(reg >> 2) & 0x03] = write.value & 0x7F;
		break;

	case 0xA0:		// 0xA0-0xA2 low f-number, 0xA4-0xA6 high f-number and block
		if ((reg & 0x08) != 0)
		{
			break;	// 0xA8-0xAE are the channel-3 special-mode registers
		}

		if ((reg & 0x04) == 0)
		{
			state.fnum = (state.fnum & 0x700) | (int) write.value;
		}
		else
		{
			state.fnum = (state.fnum & 0x0FF) | ((int) (write.value & 0x07) << 8);
			state.block = (int) ((write.value >> 3) & 0x07);
		}
		break;

	case 0xB0:		// feedback in the high bits, algorithm in the low three
		state.algorithm = (int) (write.value & 0x07);
		break;

	default:
		break;
	}
}

void Ym2612Decoder::advance (std::uint32_t)
{
	// No timers: an OPN note lasts exactly as long as the key register says, apart from the
	// release tail, which is not modelled.
}

ChannelState Ym2612Decoder::channelState (int channel) const
{
	ChannelState state;
	if (channel < 0 || channel >= numChannels)
	{
		return state;
	}

	const Impl::Channel& voice = impl->channels[channel];
	state.present = true;
	state.period = (std::uint32_t) voice.fnum;
	state.block = voice.block;

	const int level = impl->loudestCarrier (voice);
	const bool dacOwnsChannel = channel == 5 && impl->dacEnabled;
	state.gate = voice.keyed && level < 0x7F && ! dacOwnsChannel;
	state.volume = 15 - (level * 15) / 127;

	return state;
}

const NoteTable& Ym2612Decoder::noteTable (int) const
{
	return impl->table;
}

} // namespace toybox::chips
