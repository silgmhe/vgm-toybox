#include "OplDecoder.h"

#include <cmath>
#include <cstdint>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	// A rhythm instrument or a Delta-T sample has no pitch of its own: C4 marks the hit.
	constexpr int kOneShotNote = 60;
	constexpr std::uint32_t kOneShotPeriod = 0x80000000u;

	constexpr std::uint32_t kRhythmGate = 4410;		// 100 ms at 44.1 kHz
	constexpr std::uint32_t kAdpcmGate = 5292;		// 120 ms

	class OplNoteTable final : public NoteTable
	{
	public:
		void configure (double chipClock, double clockDivisor, int octaveReference)
		{
			clock = chipClock > 0.0 ? chipClock : 3579545.0;
			divisor = clockDivisor;
			reference = octaveReference;
		}

		int noteFor (std::uint32_t fnum, int block, int* finePitch) const override
		{
			if ((fnum & kOneShotPeriod) != 0)
			{
				*finePitch = 0;
				return kOneShotNote;
			}

			if (fnum == 0)
			{
				*finePitch = 0;
				return -1;
			}

			const double hz = (double) fnum * clock / divisor
							  * std::exp2 ((double) block - (double) reference);
			return quantiseFrequency (hz, finePitch);
		}

	private:
		double clock = 3579545.0;
		double divisor = 72.0;
		int reference = 20;
	};
}

struct OplDecoder::Impl
{
	struct Channel
	{
		int fnum = 0;
		int block = 0;
		bool keyed = false;
	};

	/** A level-triggered hit: the rising edge sets the end time, the chip itself ends it. */
	struct Pulse
	{
		std::uint32_t end = 0;
	};

	Variant variant = Variant::opl2;
	bool adpcm = false;
	Channel channels[18];
	Pulse rhythm[5];
	Pulse deltaT;
	std::uint8_t rhythmBits = 0;
	bool rhythmEnabled = false;
	std::uint32_t now = 0;
	OplNoteTable table;

	int channelCount = 14;
	int rhythmBase = 9;
	bool twoBanks = false;

	void configure (void)
	{
		switch (variant)
		{
		case Variant::opll:
			channelCount = 14;
			rhythmBase = 9;
			twoBanks = false;
			table.configure (0.0, 72.0, 19);
			break;

		case Variant::opl2:
			channelCount = adpcm ? 15 : 14;
			rhythmBase = 9;
			twoBanks = false;
			table.configure (0.0, 72.0, 20);
			break;

		case Variant::opl3:
		default:
			channelCount = 23;
			rhythmBase = 18;
			twoBanks = true;
			table.configure (0.0, 288.0, 20);
			break;
		}
	}

	/** Rhythm mode borrows channels 6..8 of the first bank; entering it stops whatever those
		channels still held, exactly as the chip does. */
	void rhythmWrite (std::uint32_t time, std::uint8_t data, bool enabled)
	{
		if (! enabled)
		{
			rhythmEnabled = false;
			rhythmBits = 0;
			return;
		}

		if (! rhythmEnabled)
		{
			rhythmEnabled = true;
			rhythmBits = 0;
			for (int channel = 6; channel < 9; ++channel)
			{
				channels[channel].keyed = false;
			}
		}

		const std::uint8_t bits = (std::uint8_t) (data & 0x1F);
		const std::uint8_t rising = (std::uint8_t) (bits & (std::uint8_t) ~rhythmBits);
		for (int i = 0; i < 5; ++i)
		{
			if ((rising & (0x10u >> i)) != 0)
			{
				rhythm[i].end = time + kRhythmGate;
			}
		}
		rhythmBits = bits;
	}

	void keyChannel (int channel, bool keyOn)
	{
		if (channel < 0 || channel >= channelCount)
		{
			return;
		}
		channels[channel].keyed = keyOn;
	}
};

OplDecoder::OplDecoder (Variant variant, bool adpcm) : impl (std::make_unique<Impl>())
{
	impl->variant = variant;
	impl->adpcm = adpcm;
	impl->configure();
}

OplDecoder::~OplDecoder() = default;

void OplDecoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const std::uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	const Variant variant = impl->variant;
	const bool adpcm = impl->adpcm;
	*impl = Impl();
	impl->variant = variant;
	impl->adpcm = adpcm;
	impl->configure();

	switch (variant)
	{
	case Variant::opll:
		impl->table.configure ((double) (clock > 0 ? clock : 3579545u), 72.0, 19);
		break;
	case Variant::opl2:
		impl->table.configure ((double) (clock > 0 ? clock : 3579545u), 72.0, 20);
		break;
	case Variant::opl3:
	default:
		impl->table.configure ((double) (clock > 0 ? clock : 14318180u), 288.0, 20);
		break;
	}
}

void OplDecoder::write (const RegisterWrite& write)
{
	const std::uint8_t reg = write.reg;
	const int port = write.port != 0 ? 1 : 0;
	const std::uint32_t time = write.sampleTime;

	if (impl->adpcm && port == 0 && reg == 0x07)
	{
		// Delta-T: bit 7 starts the sample; the unit ends it on its own.
		if ((write.value & 0x80) != 0 && impl->deltaT.end <= time)
		{
			impl->deltaT.end = time + kAdpcmGate;
		}
		return;
	}

	if (impl->variant == Variant::opll)
	{
		if (reg >= 0x10 && reg < 0x19)
		{
			Impl::Channel& voice = impl->channels[reg - 0x10];
			voice.fnum = (voice.fnum & 0x100) | (int) write.value;
			return;
		}

		if (reg >= 0x20 && reg < 0x29)
		{
			const int channel = reg - 0x20;
			if (impl->rhythmEnabled && channel >= 6)
			{
				return;		// borrowed by the rhythm instruments
			}

			Impl::Channel& voice = impl->channels[channel];
			voice.fnum = (voice.fnum & 0x0FF) | ((int) (write.value & 0x01) << 8);
			voice.block = (int) ((write.value >> 1) & 0x07);
			voice.keyed = (write.value & 0x10) != 0;
			return;
		}

		if (reg >= 0x30 && reg < 0x39)
		{
			// bit 5 is the key-off; the rest of the register is instrument and volume
			if ((write.value & 0x20) != 0)
			{
				impl->keyChannel (reg - 0x30, false);
			}
			return;
		}

		if (reg == 0x0E)
		{
			impl->rhythmWrite (time, write.value, (write.value & 0x20) != 0);
		}
		return;
	}

	if (reg >= 0xA0 && reg < 0xA9)
	{
		const int channel = (impl->twoBanks ? port * 9 : 0) + (reg - 0xA0);
		Impl::Channel& voice = impl->channels[channel];
		voice.fnum = (voice.fnum & 0x300) | (int) write.value;
		return;
	}

	if (reg >= 0xB0 && reg < 0xB9)
	{
		const int channel = (impl->twoBanks ? port * 9 : 0) + (reg - 0xB0);
		if (impl->rhythmEnabled && port == 0 && channel >= 6 && channel < 9)
		{
			return;		// borrowed by the rhythm instruments
		}

		Impl::Channel& voice = impl->channels[channel];
		voice.fnum = (voice.fnum & 0x0FF) | ((int) (write.value & 0x03) << 8);
		voice.block = (int) ((write.value >> 2) & 0x07);
		voice.keyed = (write.value & 0x20) != 0;
		return;
	}

	if (reg == 0xBD && port == 0)
	{
		impl->rhythmWrite (time, write.value, (write.value & 0x20) != 0);
	}
}

void OplDecoder::advance (std::uint32_t sampleTime)
{
	impl->now = sampleTime;
}

int OplDecoder::channelCount() const
{
	return impl->channelCount;
}

ChannelState OplDecoder::channelState (int channel) const
{
	ChannelState state;
	if (channel < 0 || channel >= impl->channelCount)
	{
		return state;
	}

	const int melodic = impl->twoBanks ? 18 : 9;
	if (channel < melodic)
	{
		const Impl::Channel& voice = impl->channels[channel];
		state.present = true;
		state.period = (std::uint32_t) voice.fnum;
		state.block = voice.block;
		state.gate = voice.keyed;
		state.volume = 15;
		return state;
	}

	if (channel >= impl->rhythmBase && channel < impl->rhythmBase + 5)
	{
		state.present = true;
		state.period = kOneShotPeriod;
		state.volume = 12;
		state.gate = impl->now < impl->rhythm[channel - impl->rhythmBase].end;
		return state;
	}

	if (impl->adpcm && channel == 14)
	{
		state.present = true;
		state.period = kOneShotPeriod;
		state.volume = 12;
		state.gate = impl->now < impl->deltaT.end;
	}

	return state;
}

const NoteTable& OplDecoder::noteTable (int) const
{
	return impl->table;
}

Family OplDecoder::family (int channel) const
{
	const int melodic = impl->twoBanks ? 18 : 9;
	if (channel < melodic)
	{
		return Family::fm;
	}

	if (channel >= impl->rhythmBase && channel < impl->rhythmBase + 5)
	{
		return Family::fmDrum;
	}

	return impl->adpcm && channel == 14 ? Family::adpcmDrum : Family::unknown;
}

bool OplDecoder::isDrum (int channel) const
{
	const int melodic = impl->twoBanks ? 18 : 9;
	if (channel < melodic)
	{
		return false;
	}

	return (channel >= impl->rhythmBase && channel < impl->rhythmBase + 5)
		   || (impl->adpcm && channel == 14);
}

} // namespace toybox::chips
