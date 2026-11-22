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

		int noteFor (uint32_t period, int, int* finePitch) const override
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
	/** **The six operator banks, kept as the chip wrote them.**

		`op[operator][bank]`, banks in register order: 0x40 (DT1/MUL), 0x60 (TL), 0x80 (KS/AR),
		0xA0 (AMS-EN/D1R), 0xC0 (DT2/D2R), 0xE0 (D1L/RR).

		Stored raw rather than decoded, because `VoicePatch` is a list of register writes: a struct of
		named fields would have to be turned back into writes before a note could be played, and that
		encoder would be a second authority over the same numbers - the shape this project has paid
		for twice.

		**The operator stride is eight, the channel is the low three bits** (`0x40 + op*8 + ch`).
		The OPN puts the operator in the same nibble as the channel (`0x40 + op*4 + ch`), and reading
		one layout as the other silently addresses a different operator's register.
	*/
	struct Channel
	{
		Channel()
		{
			// Total level powers up fully attenuated: a channel with no patch must be silent, not
			// loud. This is the one bank whose reset value is not zero.
			for (int op = 0; op < 4; ++op)
			{
				op_[op][1] = 0x7F;
			}
		}

		int kc = 0;
		int kf = 0;
		bool havePitch = false;
		bool keyed = false;

		uint8_t op_[4][6] {};	// the name `op` is the loop variable everywhere below
		uint8_t control = 0;	// 0x20-0x27: pan in 6-7, feedback in 3-5, connection in 0-2

		uint8_t& reg (int operatorIndex, int bank) { return op_[operatorIndex][bank]; }
		uint8_t reg (int operatorIndex, int bank) const { return op_[operatorIndex][bank]; }
	};

	/** **One captured patch per channel, rebuilt only when a register moved.** `channelState` runs
		once per channel per snapshot, and a patch is 25 pairs - building it every time would be a
		thousand allocations a second for numbers that change a few times per note. The version is
		bumped by every write that can change a patch.
	*/
	struct PatchCache
	{
		unsigned version = 0;
		std::shared_ptr<const VoicePatch> patch;
	};

	Channel channels[kChannels];
	unsigned version = 0;
	mutable PatchCache cache[kChannels];
	OpmNoteTable table;
};

OpmDecoder::OpmDecoder() : impl (std::make_unique<Impl>())
{
	impl->table.setClock (0.0);
}

OpmDecoder::~OpmDecoder() = default;

void OpmDecoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	*impl = Impl();
	impl->table.setClock ((double) clock);
}

void OpmDecoder::write (const RegisterWrite& write)
{
	const uint8_t reg = write.reg;

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
		return;
	}

	// 0x20-0x27: pan in bits 6-7, feedback in bits 3-5, connection (the algorithm that decides
	// which operators reach the output) in the low three.
	if (reg >= 0x20 && reg < 0x28)
	{
		impl->channels[reg & 0x07].control = (uint8_t) write.value;
		++impl->version;
		return;
	}

	// **0x40-0xFF: six banks of four operators.** The bank is the top three bits, counting from
	// 0x40; the operator is bits 3-4; the channel is the low three. One branch for all six
	// because the decoder keeps them raw - a field per bank would be six copies of this mapping.
	if (reg >= 0x40)
	{
		const int bank = (int) ((reg >> 5) & 0x07) - 2;	// 0x40 -> 0 ... 0xE0 -> 5
		const int op = (int) ((reg >> 3) & 0x03);
		impl->channels[reg & 0x07].op_[op][bank] = (uint8_t) write.value;

		// **Total level is a volume, not a shape, and it does not invalidate the picture.** The
		// other five banks decide the envelope and the timbre; `0x60` only decides how loud the
		// result comes out, which `noteWave::render` normalises away. Bumping the version for it
		// meant rebuilding eight 12 800-sample waveforms whenever a song rewrote a level - and on
		// this chip that is every frame. Measured as audio-thread stutter before this line.
		if (bank != 1)
		{
			++impl->version;
		}
	}
}

void OpmDecoder::advance (uint32_t)
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
	state.period = (uint32_t) ((voice.kc << 6) | voice.kf);

	// **A level, not a flag.** This was the constant 15, so every row's meter sat at full for the
	// whole song. The OPNA's version reads the loudest *carrier*, which needs the algorithm;
	// operator 4 (C2) is the carrier under CON 0-4 and one of them under 5-7, so its level is a
	// close enough answer here - and it is a display quantity, because the patch carries the TL
	// registers and the core does the arithmetic.
	state.volume = 15 - ((int) voice.reg (3, 1) * 15) / 127;

	// The patch, for the display only. Operator 2 is the modulator on every algorithm but 7, and
	// the YM2151's operators have one waveform each, so `fmWaveform` stays zero.
	state.fmKnown = true;
	state.fmMultiple = (int) (voice.reg (1, 0) & 0x0F);
	state.fmLevel = ((int) voice.reg (1, 1) * 63) / 127;
	state.fmFeedback = (int) ((voice.control >> 3) & 0x07);
	state.fmWaveform = 0;

	// **The patch, rebuilt only when a register moved.** This function is called once per channel
	// per snapshot - a fresh 25-pair vector every time would be a thousand allocations a second
	// for numbers that change a few times per note.
	Impl::PatchCache& cache = impl->cache[channel];

	if (cache.version != impl->version || cache.patch == nullptr)
	{
		chips::VoicePatch captured;
		cache.patch = capturePatch (channel, captured)
						  ? std::make_shared<const chips::VoicePatch> (std::move (captured))
						  : nullptr;
		cache.version = impl->version;
	}

	state.voicePatch = cache.patch;
	return state;
}

bool OpmDecoder::capturePatch (int channel, VoicePatch& out) const
{
	if (channel < 0 || channel >= kChannels)
	{
		return false;
	}

	const Impl::Channel& voice = impl->channels[channel];

	out = VoicePatch {};
	out.model = ChipModel::ym2151;

	// **25 pairs: six banks of four operators, then the channel's control byte.** The offsets are
	// the chip's own, so `load()` writes them back in order with nothing to encode in between.
	//
	// `+ op * 8`, not the OPN's `+ op * 4`: the operator stride differs between the two families
	// and reusing the OPN's would send channel 4's patch over channel 0's registers.
	for (int op = 0; op < 4; ++op)
	{
		for (int bank = 0; bank < 6; ++bank)
		{
			out.regs.push_back ({ (uint16_t) (0x40 + bank * 0x20 + op * 8 + channel),
								  voice.reg (op, bank) });
		}
	}

	out.regs.push_back ({ (uint16_t) (0x20 + channel), voice.control });
	return true;
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
