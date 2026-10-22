#include "OpnDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	// A channel with no reliable pitch (DAC, ADPCM) still gets a MIDI note: C4 is a trigger
	// marker, and the period carries a flag the note table recognises.
	constexpr int kOneShotNote = 60;

	// The DELTA-N value an ADPCM-B ROM is taken to be recorded at - 0x4000 is the figure the sets
	// use for an untransposed sample, and the only reference the chip offers: DELTA-N itself is a
	// playback rate, and no register names a root. It is therefore the reference of the
	// transposition layer (L1), not a claim about the recording - see design doc section 14.3.
	constexpr std::uint16_t kAdpcmBReferenceDelta = 0x4000;
	constexpr std::uint32_t kOneShotPeriod = 0x80000000u;

	/** One bit per operator that reaches the output, in register order (0x30, 0x34, 0x38, 0x3C).
		Taken from the core's own per-algorithm early-out: algorithms 0-3 have a single carrier,
		4 has two, 5 and 6 have three, and 7 sums all four. */
	constexpr std::uint8_t kCarriers[8] = { 0x08, 0x08, 0x08, 0x08,
											0x0C, 0x0E, 0x0E, 0x0F };

	// Above C8 a "period" is an artifact, as on every other sampled player here.
	constexpr int kHighestMelodicNote = 108;

	/** The ADPCM-B nibble decoder from the core's ymdeltat.c: a 4-bit delta picks a step from
		table B1 scaled by the current step size, and table B2 scales the step size for the next
		nibble. The measured period comes from the running sum, exactly as the DPCM voices of the
		K054539 do. */
	constexpr int kAdpcmBStepB1[16] = { 1, 3, 5, 7, 9, 11, 13, 15,
										-1, -3, -5, -7, -9, -11, -13, -15 };
	constexpr int kAdpcmBStepB2[16] = { 57, 57, 57, 57, 77, 102, 128, 153,
										57, 57, 57, 57, 77, 102, 128, 153 };
	constexpr int kAdpcmBStepMin = 127;
	constexpr int kAdpcmBStepMax = 24576;

	/** The chip masks its ROM address with the size rounded up to a power of two, the way the
		QSound does - a ROM that is not a power of two therefore wraps rather than reads zero. */
	std::uint32_t pow2Mask (std::size_t size)
	{
		if (size == 0)
		{
			return 0;
		}

		std::uint32_t mask = (std::uint32_t) (size - 1);
		mask |= mask >> 1;
		mask |= mask >> 2;
		mask |= mask >> 4;
		mask |= mask >> 8;
		mask |= mask >> 16;
		return mask;
	}

	/** ADPCM-B's pitch is DELTA-N, not a register a driver can ignore: the unit plays
		`delta * clock / 65536` bytes a second (the core advances `delta * clock / rate / 65536`
		bytes per output sample), and one byte is two nibbles. The sample's own cycle - measured
		from the 0x82 ROM once per start address - turns that into a note, so this is the root the
		doc calls the "ADPCM-B root". A sample with no period is a drum and stays a C4 trigger. */
	class OpnAdpcmBNoteTable final : public NoteTable
	{
	public:
		/** `nibblesPerDelta` is `(clock / 144) / 65536` - the DAC rate over the DELTA-N scale, the
			same relation `adpcmBUnitsPerSecond()` in the state uses. The two used to disagree (this
			one was `clock / 32768`, 288 times too fast), which put every real Neo Geo note past the
			top of the MIDI range and left the channel silent. */
		OpnAdpcmBNoteTable (double chipClock, const double* sampleCycle, const bool* sampleMelodic)
			: nibblesPerDelta (chipClock > 0.0 ? chipClock / (144.0 * 65536.0)
											   : 8000000.0 / (144.0 * 65536.0))
			, cycleLength (sampleCycle)
			, melodic (sampleMelodic) {}

		/** DELTA-N is a pitch register, so the note never depends on the sample being measurable:
			the rate goes against 8 kHz worth of nibbles, which is the reference the reference
			implementation divides an ADPCM rate by (`fSample / 8000`, `frmY8950.cs`). The sample's
			own cycle rides beside it as the root correction, and a set whose ROM is missing simply
			loses that correction rather than its pitch. */
		int noteFor (std::uint32_t delta, int, int* finePitch) const override
		{
			if (delta == 0)
			{
				*finePitch = 0;
				return kOneShotNote;		// DELTA-N 0 is silence, not a note
			}

			return quantisePlaybackRate ((double) delta * nibblesPerDelta,
										 kSampleReferenceRate, finePitch);
		}

	private:
		double nibblesPerDelta;
		const double* cycleLength;
		const bool* melodic;
	};

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

	/** ADPCM-B: the one sampled voice with a real pitch register (DELTA-N), an end address and a
		repeat bit. Everything the note needs is latched when the control register's start bit goes
		up, because the driver rewrites start/end/delta for the next sample before the next start. */
	struct AdpcmB
	{
		std::uint16_t startReg = 0;
		std::uint16_t endReg = 0;
		std::uint16_t delta = 0;
		std::uint8_t control = 0;
		std::uint8_t level = 0xFF;

		std::uint32_t latchStart = 0;
		std::uint32_t latchEnd = 0;
		bool active = false;
		bool repeat = false;
		std::uint32_t gateEnd = 0;
		double cycleLength = 0.0;
		bool melodic = false;
		std::uint32_t keyOns = 0;
	};

	Variant variant = Variant::ym2612;
	Channel channels[6];
	Pulse adpcmA[6];
	AdpcmB adpcmB;
	Pulse dac;
	std::uint32_t lastDacTrigger = 0;
	bool dacTriggered = false;
	bool dacEnabled = false;
	std::uint32_t now = 0;
	double clock = 8000000.0;
	double streamRate = 44100.0;
	std::vector<std::uint8_t> adpcmBRom;
	std::uint32_t adpcmBMask = 0;
	std::map<std::uint32_t, double> adpcmBCycleCache;
	std::unique_ptr<OpnAdpcmBNoteTable> adpcmBTable;
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

		// the table holds pointers into this object, so it has to outlive a reset, not be reset
		if (hasAdpcm())
		{
			adpcmBTable = std::make_unique<OpnAdpcmBNoteTable> (clock, &adpcmB.cycleLength,
															   &adpcmB.melodic);
		}
		else
		{
			adpcmBTable.reset();
		}
	}

	/** The core's ymdeltat portshift: the same 16-bit start/end register addresses 256-byte pages
		on the YM2610 and 32-byte ones on the OPNA. */
	int adpcmBShift (void) const
	{
		return (variant == Variant::ym2610 || variant == Variant::ym2610b) ? 8 : 5;
	}

	std::uint32_t adpcmBAddress (std::uint16_t reg) const
	{
		return (std::uint32_t) reg << adpcmBShift();
	}

	std::uint8_t adpcmBByte (std::uint32_t address) const
	{
		const std::uint32_t index = address & adpcmBMask;
		return index < adpcmBRom.size() ? adpcmBRom[index] : 0;
	}

	/** ADPCM-B nibbles a second at this DELTA-N.

		The chip's ADPCM-B DAC runs at `clock / 144` and DELTA-N is a 16-bit fraction of it, so a
		sample advances `delta / 65536` of a nibble per DAC tick:

			rate = delta * (clock / 144) / 65536

		which is the same relation the reference implementation uses (`deltaN * 50kHz / 2^16`,
		`form/KB/OPL/frmY8950.cs` - its 50 kHz is exactly `clock/144` on a 7.2 MHz part). This used
		to be `delta * clock / 32768`, i.e. 288 times too fast: the gate then closed within a single
		sample and every Neo Geo ADPCM-B track came out empty, and the note was out of range besides. */
	double adpcmBUnitsPerSecond (std::uint16_t delta) const
	{
		return (double) delta * clock / (144.0 * 65536.0);
	}

	/** The sample's own cycle in nibbles, measured once per start address: the ADPCM-B ROM is
		4-bit delta data, so it is decoded first - the period only exists in the running sum. */
	double adpcmBCycleFor (std::uint32_t start)
	{
		if (adpcmBRom.empty())
		{
			return 0.0;
		}

		const auto cached = adpcmBCycleCache.find (start);
		if (cached != adpcmBCycleCache.end())
		{
			return cached->second;
		}

		constexpr std::size_t kBytes = 4096;
		std::vector<double> waveform;
		waveform.reserve (kBytes * 2);

		double accumulator = 0.0;
		int step = kAdpcmBStepMin;
		for (std::size_t i = 0; i < kBytes; ++i)
		{
			const int byte = adpcmBByte (start + (std::uint32_t) i);
			for (int half = 0; half < 2; ++half)
			{
				const int nibble = (half == 0) ? ((byte >> 4) & 0x0F) : (byte & 0x0F);
				accumulator += (double) (kAdpcmBStepB1[nibble] * step / 8);
				accumulator = std::clamp (accumulator, -32768.0, 32767.0);
				waveform.push_back (accumulator);
				step = std::clamp (step * kAdpcmBStepB2[nibble] / 64, kAdpcmBStepMin,
								   kAdpcmBStepMax);
			}
		}

		const double cycle = estimateCycleLength (waveform, 0, 0);
		adpcmBCycleCache[start] = cycle;
		return cycle;
	}

	void startAdpcmB (void)
	{
		AdpcmB& voice = adpcmB;
		voice.latchStart = adpcmBAddress (voice.startReg);
		voice.latchEnd = adpcmBAddress (voice.endReg) + ((1u << adpcmBShift()) - 1u);
		voice.repeat = (voice.control & 0x10) != 0;
		voice.active = true;
		++voice.keyOns;

		voice.cycleLength = adpcmBCycleFor (voice.latchStart);

		// DELTA-N is a pitch register, so every keyed voice with a non-zero DELTA-N is melodic -
		// the sample only decides whether a root correction can be added on top. Requiring a
		// measurable period made the whole channel one-shot whenever the set's ROM was absent or
		// its attack was noisy, which is what this used to do.
		voice.melodic = voice.delta != 0;
		if (voice.melodic && voice.cycleLength > 0.0 && ! adpcmBRom.empty())
		{
			int fine = 0;
			const int note =
				quantiseFrequency (adpcmBUnitsPerSecond (voice.delta) / voice.cycleLength, &fine);

			// a measured pitch that lands out of range is a formant, not a note: keep the register
			// pitch and drop the root correction rather than the voice
			if (note < 0 || note > kHighestMelodicNote)
			{
				voice.cycleLength = 0.0;
			}
		}

		// The level register is what the driver actually closes the note with: the Neo Geo stage
		// theme writes level 0 before every new start, and a note that ran to its end address
		// instead would be cut at a length the register cannot be trusted for. The end address
		// stays in latchEnd so a driver that never silences still stops - see channelState().
		voice.gateEnd = 0;

	}

	/** The ADPCM-B block, at the offsets of the core's ymdeltat: 0x00 control, 0x02/0x03 start,
		0x04/0x05 end, 0x09/0x0A DELTA-N, 0x0B level. */
	void writeAdpcmB (std::uint8_t offset, std::uint8_t value)
	{
		switch (offset)
		{
		// ADPCM-B's own register block - the "Delta-T" voice the YM2608 and YM2610 share. The
		// offsets inside it, which is all the numbers below mean:
		//
		//   0x00  control: bit 7 starts, bit 6 repeats, bit 0 resets; the key edge is here
		//   0x02/0x03  start address in the sample ROM, low byte first
		//   0x04/0x05  end address, which is what stops the voice
		//   0x09/0x0A  DELTA-N, the playback rate - the chip's only pitch register
		//   0x0B  output level (an attenuation; a zero write is how these sets end a voice)
		//   0x0D  LFO, which modulates the pitch the rate already gave
		case 0x00:
			keyAdpcmB (value);
			break;
		case 0x02:
			adpcmB.startReg = (std::uint16_t) ((adpcmB.startReg & 0xFF00) | value);
			break;
		case 0x03:
			adpcmB.startReg = (std::uint16_t) ((adpcmB.startReg & 0x00FF) | (value << 8));
			break;
		case 0x04:
			adpcmB.endReg = (std::uint16_t) ((adpcmB.endReg & 0xFF00) | value);
			break;
		case 0x05:
			adpcmB.endReg = (std::uint16_t) ((adpcmB.endReg & 0x00FF) | (value << 8));
			break;
		case 0x09:
			adpcmB.delta = (std::uint16_t) ((adpcmB.delta & 0xFF00) | value);
			break;
		case 0x0A:
			adpcmB.delta = (std::uint16_t) ((adpcmB.delta & 0x00FF) | (value << 8));
			break;
		case 0x0B:
			adpcmB.level = value;
			if (value == 0)
			{
				adpcmB.active = false;		// level 0 is silence, and it is how drivers end a note
			}
			break;
		default:
			break;
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

	void keyAdpcmB (std::uint8_t data)
	{
		// Bit 7 starts the sample and reloads the address, so a write with it on is a key-on even
		// when the voice was already sounding; bit 0 resets the unit.
		adpcmB.control = data;
		if ((data & 0x80) != 0)
		{
			startAdpcmB();
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

void OpnDecoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const std::uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	const Variant variant = impl->variant;

	// the ROM belongs to the song, not to the clock
	std::vector<std::uint8_t> rom = std::move (impl->adpcmBRom);
	const std::uint32_t mask = impl->adpcmBMask;

	*impl = Impl();
	impl->variant = variant;
	impl->adpcmBRom = std::move (rom);
	impl->adpcmBMask = mask;
	impl->clock = clock > 0 ? (double) clock : 8000000.0;
	impl->streamRate = sampleRate > 0.0 ? sampleRate : 44100.0;
	impl->configure();
	impl->table.setClock ((double) clock);
}

void OpnDecoder::setAdpcmBRom (std::vector<std::uint8_t> rom)
{
	impl->adpcmBMask = pow2Mask (rom.size());
	impl->adpcmBRom = std::move (rom);
	impl->adpcmBCycleCache.clear();
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
			impl->writeAdpcmB (reg, write.value);
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
			// the OPNB's ADPCM-B block sits at 0x10 where the OPNA's sits at 0x00
			impl->writeAdpcmB ((std::uint8_t) (reg - 0x10), write.value);
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

	if (isDac)
	{
		state.period = kOneShotPeriod;
		state.volume = 12;		// a trigger marker, not a measured level
		state.gate = impl->now < impl->dac.end;
		return state;
	}

	if (channel < impl->fmCount + 6)
	{
		state.period = kOneShotPeriod;
		state.volume = 12;
		state.gate = impl->now < impl->adpcmA[channel - impl->fmCount].end;
		return state;
	}

	// ADPCM-B is the one sampled voice with a pitch register: DELTA-N is the note, the level
	// register the volume, and the start bit the gate
	const Impl::AdpcmB& voice = impl->adpcmB;
	state.period = std::max<std::uint32_t> (voice.delta, 1u);
	state.volume = (int) ((voice.level * 15u) / 255u);
	state.keyOns = voice.keyOns;
	// The level register is an offset, not a switch: the Neo Geo sets leave it at zero while the
	// channel plays loudly (0x1B is only written by drivers that attenuate), so gating on it
	// silenced every one of them. The start bit and the end address decide whether it sounds.
	state.gate = voice.active && (voice.gateEnd == 0 || impl->now < voice.gateEnd);

	// L1 from DELTA-N against the 8 kHz reference, L2 from the sample's own cycle: the sets name no
	// recording rate, so the transposition is the only register-only statement and the root is what
	// places the octave (design doc section 14.3)
	const SampledPitch pitch = describeSamplePitch (impl->adpcmBUnitsPerSecond (voice.delta),
													kSampleReferenceRate, voice.cycleLength,
													voice.melodic);
	state.hz = pitch.hz;
	state.transposeCents = pitch.transposeCents;
	state.rootCents = pitch.rootCents;
	state.pitchSource = pitch.source;

	return state;
}

const NoteTable& OpnDecoder::noteTable (int channel) const
{
	if (impl->adpcmBTable != nullptr && channel == impl->fmCount + 6)
	{
		return *impl->adpcmBTable;
	}

	return impl->table;
}

Family OpnDecoder::family (int channel) const
{
	if (channel < impl->fmCount)
	{
		return Family::fm;
	}

	return isDrum (channel) ? Family::adpcmDrum : Family::opnAdpcmB;
}

bool OpnDecoder::isDrum (int channel) const
{
	if (channel < impl->fmCount)
	{
		return false;
	}

	// ADPCM-A is a trigger; ADPCM-B is melodic when its sample has a period
	return ! (channel == impl->fmCount + 6 && impl->adpcmB.melodic);
}

} // namespace toybox::chips
