#include "Ymf271Decoder.h"

#include <algorithm>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	// Most boards run the OPX at 16.9344 MHz, the same clock as the OPL4 next to it.
	constexpr uint32_t kDefaultClock = 16934400;

	constexpr int kBanks = 4;
	constexpr int kSlotsPerBank = 12;
	constexpr int kSlots = kBanks * kSlotsPerBank;

	constexpr int kOneShotNote = 60;
	constexpr uint32_t kOneShotPeriod = 0x80000000u;

	// A PCM slot has no length register that means anything in time (start/end/loop are sample
	// addresses), so the gate is a fixed window from the key-on edge - the X1-010's convention.
	constexpr uint32_t kPcmGateSamples = 3528;		// 80 ms at 44.1 kHz

	// ymf271.c: pow_table / multiple_table, and `rate = clock / 384` at line 1868.
	constexpr double kPowTable[16] = { 128, 256, 512, 1024, 2048, 4096, 8192, 16384,
									   0.5, 1, 2, 4, 8, 16, 32, 64 };
	constexpr double kMultipleTable[16] = { 0.5, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
	constexpr double kStepDivisor = 268435456.0;		// 2^28

	// Channel output levels 13..15 are -96 dB in the core's attenuation table, i.e. silence.
	constexpr int kSilentLevel = 13;

	// ymf271.c fm_tab: the low nibble of an FM address is the group, and 3/7/11/15 are holes.
	constexpr int kFmGroup[16] = { 0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1 };

	/** Which slots of a group reach the output under an algorithm.

		Bit n is bank n (slot n + 1 in the core's diagrams). The tables are read off the switches
		in ymf271_update: four-operator algorithms 0-5 end in S4 only, 6/7 add S3, 8/9 add S1,
		10-12 add S2 and S3, 13-15 pick the remaining combinations; the two-operator algorithm
		only looks at its low two bits (0/1 = carrier S3, 2/3 = both); the three-operator one
		uses three bits (0-2 = S2 only, 3 = S1+S2, 4/5 = S2+S3, 6/7 = all three). */
	constexpr uint8_t kCarriers4[16] = { 0x8, 0x8, 0x8, 0x8, 0x8, 0x8, 0xC, 0xC,
											  0x9, 0x9, 0xE, 0xE, 0xE, 0xB, 0xD, 0xF };
	constexpr uint8_t kCarriers2[4] = { 0x2, 0x2, 0x3, 0x3 };
	constexpr uint8_t kCarriers3[8] = { 0x2, 0x2, 0x2, 0x3, 0x6, 0x6, 0x7, 0x7 };

	bool isCarrier (int sync, int algorithm, int bank)
	{
		switch (sync)
		{
		case 0:
			return ((kCarriers4[algorithm & 0x0F] >> bank) & 1) != 0;

		case 1:
		{
			// pair A is banks 0/2, pair B is banks 1/3, each pair answering to its own alg
			const int local = bank >> 1;
			return ((kCarriers2[algorithm & 0x03] >> local) & 1) != 0;
		}

		case 2:
			return bank < 3 && ((kCarriers3[algorithm & 0x07] >> bank) & 1) != 0;

		default:
			return false;
		}
	}

	class Ymf271NoteTable final : public NoteTable
	{
	public:
		explicit Ymf271NoteTable (double nativeRate) : nativeRate (nativeRate) {}

		int noteFor (uint32_t period, int block, int* finePitch) const override
		{
			if ((period & kOneShotPeriod) != 0)
			{
				if (finePitch != nullptr)
				{
					*finePitch = 0;
				}

				return kOneShotNote;
			}

			const uint32_t fns = period & 0x0FFFu;
			if (fns == 0)
			{
				return -1;		// a phase increment of zero is silence, not a very low note
			}

			const int multiple = (int) ((period >> 12) & 0x0Fu);
			const double hz = nativeRate * (double) fns * kPowTable[block & 0x0F]
								 * kMultipleTable[multiple] / kStepDivisor;
			return quantiseFrequency (hz, finePitch);
		}

	private:
		double nativeRate;
	};
}

struct Ymf271Decoder::Impl
{
	struct Slot
	{
		bool keyed = false;			// 0x0 bit 0; the core's "active" also waits for the envelope
		bool gateOpen = false;		// PCM one-shot window
		uint32_t gateEnd = 0;
		uint8_t fnsHi = 0;		// 0xA: block in the high nibble, fns bits 8-11 below it
		uint16_t fns = 0;		// latched by 0x9, like the core
		uint8_t block = 0;
		uint8_t multiple = 0;	// 0x3
		uint8_t tl = 0;		// 0x4
		uint8_t waveform = 0;	// 0xB low nibble - the patch number this decoder reports
		uint8_t algorithm = 0;	// 0xC
		uint8_t ch0Level = 0;	// 0xD
		uint8_t ch1Level = 0;
	};

	struct Group
	{
		uint8_t sync = 0;		// 0x0/0x1/0x2/0x3 on the timer port; reset value is 4-op FM
	};

	Slot slots[kSlots];
	Group groups[kSlotsPerBank];
	uint32_t now = 0;
	double clock = (double) kDefaultClock;
	std::unique_ptr<Ymf271NoteTable> table;

	void applySlot (int slotIndex, int reg, uint8_t value, uint32_t time);
};

void Ymf271Decoder::Impl::applySlot (int slotIndex, int reg, uint8_t value,
									 uint32_t time)
{
	Slot& slot = slots[slotIndex];
	const int group = slotIndex % kSlotsPerBank;

	switch (reg)
	{
	case 0x0:
	{
		const bool wasKeyed = slot.keyed;
		slot.keyed = (value & 0x01) != 0;

		if (! slot.keyed)
		{
			slot.gateOpen = false;
		}
		else if (! wasKeyed && groups[group].sync == 3)
		{
			slot.gateEnd = time + kPcmGateSamples;
			slot.gateOpen = true;
		}

		break;
	}

	// One slot's parameter block, by offset inside it:
	//
	//   0x0  key-on and the slot's control bits (the edge is what starts a note)
	//   0x3  MULTIPLE, the operator's frequency multiplier
	//   0x4  TL, total level - attenuation from the loudest, so a large value is quiet
	//   0x9/0xA  FNS, the frequency number: low eight bits at 0x9, and the high byte at 0xA also
	//            carries the block (octave), which is why both are latched together
	//   0xB  waveform select
	//   0xC  algorithm, which decides how many operators a note really uses
	//   0xD  channel 0 and 1 levels, one nibble each
	case 0x3:
		slot.multiple = value & 0x0F;
		break;

	case 0x4:
		slot.tl = value & 0x7F;
		break;

	case 0x9:
		// the core latches both halves here and reads the block out of the high byte
		slot.fns = (uint16_t) (((slot.fnsHi << 8) & 0x0F00) | value);
		slot.block = (uint8_t) ((slot.fnsHi >> 4) & 0x0F);
		break;

	case 0xA:
		slot.fnsHi = value;
		break;

	case 0xB:
		slot.waveform = value & 0x07;
		break;

	case 0xC:
		slot.algorithm = value & 0x0F;
		break;

	case 0xD:
		slot.ch0Level = (uint8_t) (value >> 4);
		slot.ch1Level = (uint8_t) (value & 0x0F);
		break;

	default:
		break;		// envelope, LFO and the ch2/ch3 levels do not change the note
	}
}

Ymf271Decoder::Ymf271Decoder() : impl (std::make_unique<Impl>())
{
	reset (ChipConfig { kDefaultClock, 44100.0 });
}

Ymf271Decoder::~Ymf271Decoder() = default;

void Ymf271Decoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	(void) sampleRate;		// the one-shot window is counted in 44.1 kHz samples, the chip has no other time base

	impl = std::make_unique<Impl>();
	impl->clock = (clock > 100000) ? (double) clock : (double) kDefaultClock;
	impl->table = std::make_unique<Ymf271NoteTable> (impl->clock / 384.0);
}

void Ymf271Decoder::write (const RegisterWrite& w)
{
	const uint8_t port = (uint8_t) (w.port & 0x7F);

	if (port <= 3)
	{
		// The FM register files: port 0 also propagates the synced registers to the other banks.
		const int group = kFmGroup[w.reg & 0x0F];
		if (group < 0)
		{
			return;
		}

		const int reg = (w.reg >> 4) & 0x0F;
		const bool synced = (reg == 0x0 || reg == 0x9 || reg == 0xA || reg == 0xC
							 || reg == 0xD || reg == 0xE);

		int keyBank = -1;		// the bank whose writes the group copies, -1 for no propagation
		switch (impl->groups[group].sync)
		{
		case 0:
			keyBank = 0;
			break;

		case 1:
			keyBank = (port <= 1) ? (int) port : -1;
			break;

		case 2:
			keyBank = 0;
			break;

		default:
			break;		// sync 3 is four independent PCM slots: every write is its own
		}

		if (! synced || (int) port != keyBank)
		{
			impl->applySlot (port * kSlotsPerBank + group, reg, w.value, w.sampleTime);
			return;
		}

		switch (impl->groups[group].sync)
		{
		case 0:
			for (int bank = 0; bank < kBanks; ++bank)
			{
				impl->applySlot (bank * kSlotsPerBank + group, reg, w.value, w.sampleTime);
			}
			break;

		case 1:
			// pair A is banks 0 / 2, pair B is banks 1 / 3
			impl->applySlot (port * kSlotsPerBank + group, reg, w.value, w.sampleTime);
			impl->applySlot ((port + 2) * kSlotsPerBank + group, reg, w.value, w.sampleTime);
			break;

		case 2:
			for (int bank = 0; bank < 3; ++bank)
			{
				impl->applySlot (bank * kSlotsPerBank + group, reg, w.value, w.sampleTime);
			}
			break;

		default:
			break;
		}

		return;
	}

	if (port == 6)
	{
		// Timer and group control share one register file; 0x00-0x0F are the group sync modes.
		if ((w.reg & 0xF0) != 0)
		{
			return;
		}

		const int group = kFmGroup[w.reg & 0x0F];
		if (group < 0)
		{
			return;
		}

		impl->groups[group].sync = (uint8_t) (w.value & 0x03);
		return;
	}

	// port 4 is the PCM register file (start/end/loop/format); none of it says when a sample ends,
	// and port 5/7 are not decoded by the core either.
}

void Ymf271Decoder::advance (uint32_t sampleTime)
{
	impl->now = sampleTime;

	for (auto& slot : impl->slots)
	{
		if (slot.gateOpen && sampleTime >= slot.gateEnd)
		{
			slot.gateOpen = false;
		}
	}
}

ChannelState Ymf271Decoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= channelCount_)
	{
		return state;
	}

	state.present = true;

	const int sync = impl->groups[channel].sync;

	if (sync == 3)
	{
		// PCM: no pitch, no readable end - the decoder's own 80 ms gate is the note
		for (int bank = 0; bank < kBanks; ++bank)
		{
			if (impl->slots[bank * kSlotsPerBank + channel].gateOpen)
			{
				state.gate = true;
				state.period = kOneShotPeriod;
				state.volume = 15;
				return state;
			}
		}

		return state;
	}

	// FM: a keyed carrier that is not silent is the voice. When several carriers answer, the
	// loudest one decides the pitch and the velocity - a modulator never does.
	int bestBank = -1;
	int bestTl = 0x80;

	for (int bank = 0; bank < kBanks; ++bank)
	{
		const Impl::Slot& slot = impl->slots[bank * kSlotsPerBank + channel];
		if (! slot.keyed)
		{
			continue;
		}

		if (! isCarrier (sync, slot.algorithm, bank))
		{
			continue;
		}

		if (slot.tl >= 0x7F)
		{
			continue;		// maximum attenuation: the operator contributes nothing
		}

		if (std::min (slot.ch0Level, slot.ch1Level) >= kSilentLevel)
		{
			continue;		// the group's two outputs are both muted
		}

		if (slot.tl < bestTl)
		{
			bestTl = slot.tl;
			bestBank = bank;
		}
	}

	if (bestBank < 0)
	{
		return state;
	}

	const Impl::Slot& voice = impl->slots[bestBank * kSlotsPerBank + channel];
	state.gate = true;
	state.block = voice.block;
	state.period = (uint32_t) voice.fns | ((uint32_t) (voice.multiple & 0x0F) << 12);
	state.volume = 15 - (bestTl * 15) / 127;
	state.patch = voice.waveform;
	return state;
}

const NoteTable& Ymf271Decoder::noteTable (int channel) const
{
	(void) channel;		// one table: the PCM marker and the FM pitch share the period word
	return *impl->table;
}

ChannelKind Ymf271Decoder::channelKind (int channel) const
{
	const bool pcm = channel >= 0 && channel < channelCount_
					 && impl->groups[channel].sync == 3;
	return pcm ? ChannelKind::adpcmDrum : ChannelKind::fm;
}

bool Ymf271Decoder::isDrum (int channel) const
{
	return channel >= 0 && channel < channelCount_ && impl->groups[channel].sync == 3;
}

} // namespace toybox::chips
