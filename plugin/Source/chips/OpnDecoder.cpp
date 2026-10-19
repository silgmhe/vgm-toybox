#include "OpnDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	// A channel with no reliable pitch (DAC, ADPCM) still gets a MIDI note: C4 is a trigger
	// marker, and the period carries a flag the note table recognises.
	constexpr int kOneShotNote = 60;
	constexpr std::uint32_t kOneShotPeriod = 0x80000000u;

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
			if ((fnum & kOneShotPeriod) != 0)
			{
				// a sample channel has no pitch: C4 is a trigger marker, not a transcription
				*finePitch = 0;
				return kOneShotNote;
			}

			const int octave = std::clamp (block, 0, 7);
			const double hz = (double) fnum * clock / (144.0 * std::exp2 (20.0 - (double) octave));
			return quantiseFrequency (hz, finePitch);
		}

	private:
		double clock = 7670453.0;
	};

	constexpr std::uint32_t kAdpcmAGate = 3528;		// 80 ms at 44.1 kHz
	constexpr std::uint32_t kAdpcmBGate = 4410;		// 100 ms
	constexpr std::uint32_t kDacGate = 2205;			// 50 ms
	constexpr std::uint32_t kDacInterval = 2205;		// one DAC marker per 50 ms at most
}

struct OpnDecoder::Impl
{
	struct Channel
	{
		int fnum = 0;
		int block = 0;
		int algorithm = 0;
		bool keyed = false;
		bool present = true;
		std::uint8_t totalLevel[4] = { 0x7F, 0x7F, 0x7F, 0x7F };
	};

	/** One one-shot channel: the write starts it, its own end time stops it. `active` keeps the
		control bit's history so only a rising edge triggers. */
	struct Pulse
	{
		std::uint32_t end = 0;
		bool active = false;
	};

	Variant variant = Variant::ym2612;
	Channel channels[6];
	Pulse adpcmA[6];
	Pulse adpcmB;
	Pulse dac;
	std::uint32_t lastDacTrigger = 0;
	bool dacTriggered = false;
	bool dacEnabled = false;
	std::uint32_t now = 0;
	OpnNoteTable table;

	int fmCount = 6;
	int channelCount = 7;
	bool sixChannel = true;

	bool hasAdpcm (void) const
	{
		return variant == Variant::ym2608 || variant == Variant::ym2610
			   || variant == Variant::ym2610b;
	}

	void configure (void)
	{
		switch (variant)
		{
		case Variant::ym2203:
			fmCount = 3;
			sixChannel = false;
			channelCount = 3;
			break;

		case Variant::ym2608:
		case Variant::ym2610:
		case Variant::ym2610b:
			fmCount = 6;
			sixChannel = true;
			channelCount = 13;
			break;

		case Variant::ym2612:
		default:
			fmCount = 6;
			sixChannel = true;
			channelCount = 7;
			break;
		}

		if (variant == Variant::ym2610)
		{
			// the non-B OPNA only wires up channels 1, 2, 4 and 5
			channels[0].present = false;
			channels[3].present = false;
		}
	}

	void keyEvent (std::uint8_t data)
	{
		int channel = (int) (data & 0x03);
		if (channel == 3)
		{
			return;		// 0x28 with channel 3 is not a key register
		}

		if (sixChannel && (data & 0x04) != 0)
		{
			channel += 3;
		}

		if (channel >= fmCount || ! channels[channel].present)
		{
			return;
		}

		// Bits 4-7 are the operator mask; a zero mask is the key-off. A partial mask that does
		// not include a carrier is silent, which channelState() deals with.
		channels[channel].keyed = (data & 0xF0) != 0;
	}

	void keyAdpcmA (std::uint32_t time, std::uint8_t data)
	{
		// Bit 7 stops every channel, bits 0..5 start one each.
		const bool allOff = (data & 0x80) != 0;
		for (int channel = 0; channel < 6; ++channel)
		{
			const bool on = ! allOff && (data & (1u << channel)) != 0;
			if (on && ! adpcmA[channel].active)
			{
				adpcmA[channel].end = time + kAdpcmAGate;
			}
			adpcmA[channel].active = on;
		}
	}

	void keyAdpcmB (std::uint32_t time, std::uint8_t data)
	{
		// Bit 7 starts the sample, bit 0 resets the unit.
		if ((data & 0x80) != 0)
		{
			if (! adpcmB.active)
			{
				adpcmB.end = time + kAdpcmBGate;
			}
			adpcmB.active = true;
		}
		else if ((data & 0x01) != 0)
		{
			adpcmB.active = false;
		}
	}

	void dacByte (std::uint32_t time, std::uint8_t data)
	{
		if (! dacEnabled || data == 0x80)
		{
			return;
		}

		if (dacTriggered && time - lastDacTrigger < kDacInterval)
		{
			return;
		}

		dac.end = time + kDacGate;
		lastDacTrigger = time;
		dacTriggered = true;
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

OpnDecoder::OpnDecoder (Variant variant) : impl (std::make_unique<Impl>())
{
	impl->variant = variant;
	impl->configure();
	impl->table.setClock (0.0);
}

OpnDecoder::~OpnDecoder() = default;

void OpnDecoder::reset (std::uint32_t clock, double)
{
	const Variant variant = impl->variant;
	*impl = Impl();
	impl->variant = variant;
	impl->configure();
	impl->table.setClock ((double) clock);
}

void OpnDecoder::write (const RegisterWrite& write)
{
	const std::uint8_t reg = write.reg;
	const int port = write.port != 0 ? 1 : 0;

	// Key on/off is the only register that addresses a channel across both ports.
	if (reg == 0x28 && port == 0)
	{
		impl->keyEvent (write.value);
		return;
	}

	if (impl->variant == Variant::ym2612 && port == 0)
	{
		if (reg == 0x2B)
		{
			impl->dacEnabled = (write.value & 0x80) != 0;
			return;
		}

		if (reg == 0x2A)
		{
			impl->dacByte (write.sampleTime, write.value);
			return;
		}
	}

	// ADPCM-A/B sit in different register windows on the OPNA and the OPNB; the ports below
	// are the ones libvgm's core decodes, not a guess from the write order.
	if (impl->variant == Variant::ym2608)
	{
		if (port == 0 && reg >= 0x10 && reg < 0x20)
		{
			if (reg == 0x10)
			{
				impl->keyAdpcmA (write.sampleTime, write.value);
			}
			return;
		}

		if (port == 1 && reg < 0x10)
		{
			if (reg == 0x00)
			{
				impl->keyAdpcmB (write.sampleTime, write.value);
			}
			return;
		}
	}
	else if (impl->variant == Variant::ym2610 || impl->variant == Variant::ym2610b)
	{
		if (port == 1 && reg < 0x30)
		{
			if (reg == 0x00)
			{
				impl->keyAdpcmA (write.sampleTime, write.value);
			}
			return;
		}

		if (port == 0 && reg >= 0x10 && reg < 0x20)
		{
			if (reg == 0x10)
			{
				impl->keyAdpcmB (write.sampleTime, write.value);
			}
			return;
		}
	}

	// Total level, one per operator: the only volume an FM channel has. In 0x40-0x4F the low
	// two bits select the channel inside the port's bank and bits 2-3 the operator, not the
	// other way round - swapping them sends each operator's level to a stranger's channel and
	// leaves the real carrier at the silent default (FM5 of a YM2612 song then played but had
	// no note: the channel's carriers still read 0x7F).
	if ((reg & 0xF0) == 0x40)
	{
		if ((reg & 0x03) == 3)
		{
			return;		// 0xX3/0xX7/0xXB/0xXF: channel 3's special-mode slots, not a channel
		}

		const int channel = (int) (reg & 0x03) + port * 3;
		if (channel >= impl->fmCount)
		{
			return;
		}

		impl->channels[channel].totalLevel[(reg >> 2) & 0x03] = write.value & 0x7F;
		return;
	}

	// 0xA0-0xA2 low f-number, 0xA4-0xA6 high f-number and block; 0xA8-0xAE are the channel-3
	// special-mode registers and carry no channel pitch.
	if ((reg & 0xF0) == 0xA0 && (reg & 0x08) == 0)
	{
		const int channel = (int) (reg & 0x03) + port * 3;
		if (channel >= impl->fmCount)
		{
			return;
		}

		Impl::Channel& state = impl->channels[channel];
		if ((reg & 0x04) == 0)
		{
			state.fnum = (state.fnum & 0x700) | (int) write.value;
		}
		else
		{
			state.fnum = (state.fnum & 0x0FF) | ((int) (write.value & 0x07) << 8);
			state.block = (int) ((write.value >> 3) & 0x07);
		}
		return;
	}

	// 0xB0-0xB2: feedback in the high bits, algorithm in the low three. 0xB4-0xB6 is pan,
	// which changes the stereo image rather than the note.
	if ((reg & 0xF0) == 0xB0 && (reg & 0x04) == 0)
	{
		const int channel = (int) (reg & 0x03) + port * 3;
		if (channel < impl->fmCount)
		{
			impl->channels[channel].algorithm = (int) (write.value & 0x07);
		}
	}
}

void OpnDecoder::advance (std::uint32_t sampleTime)
{
	// No timers: an OPN FM note lasts exactly as long as the key register says, and the
	// one-shots are bounded by their own end times.
	impl->now = sampleTime;
}

int OpnDecoder::channelCount() const
{
	return impl->channelCount;
}

ChannelState OpnDecoder::channelState (int channel) const
{
	ChannelState state;
	if (channel < 0 || channel >= impl->channelCount)
	{
		return state;
	}

	const bool isDac = impl->variant == Variant::ym2612 && channel == 6;
	if (channel < impl->fmCount)
	{
		const Impl::Channel& voice = impl->channels[channel];
		if (! voice.present)
		{
			return state;
		}

		state.present = true;
		state.period = (std::uint32_t) voice.fnum;
		state.block = voice.block;

		const int level = impl->loudestCarrier (voice);
		const bool dacOwnsChannel = channel == 5 && impl->dacEnabled;
		state.gate = voice.keyed && level < 0x7F && ! dacOwnsChannel;
		state.volume = 15 - (level * 15) / 127;
		return state;
	}

	if (! isDac && ! impl->hasAdpcm())
	{
		return state;
	}

	state.present = true;
	state.period = kOneShotPeriod;
	state.volume = 12;		// a trigger marker, not a measured level

	if (isDac)
	{
		state.gate = impl->now < impl->dac.end;
	}
	else if (channel < 12)
	{
		state.gate = impl->now < impl->adpcmA[channel - 6].end;
	}
	else
	{
		state.gate = impl->now < impl->adpcmB.end;
	}

	return state;
}

const NoteTable& OpnDecoder::noteTable (int) const
{
	return impl->table;
}

Family OpnDecoder::family (int channel) const
{
	return channel < impl->fmCount ? Family::fm : Family::adpcmDrum;
}

bool OpnDecoder::isDrum (int channel) const
{
	return channel >= impl->fmCount;
}

} // namespace toybox::chips
