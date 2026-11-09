#include "OpnDecoder.h"

#include <algorithm>
#include <cassert>
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
	constexpr uint16_t kAdpcmBReferenceDelta = 0x4000;
	constexpr uint32_t kOneShotPeriod = 0x80000000u;

	/** The key-on register (0x28) packs its operand. The channel is in the low two bits; bit 2
		says "all four operators" and exists only on the six-channel parts; the high nibble names
		which operators are keyed. Channel 3 is not a key register at all - the FM numbering skips
		it - which is why the channel mask is read before anything else. */
	constexpr uint8_t kKeyChannelMask = 0x03;
	constexpr uint8_t kKeyAllOperators = 0x04;
	constexpr uint8_t kKeyOperatorMask = 0xF0;

	/** The ADPCM registers that hold an address are 16 bits wide and written a byte at a time:
		each write replaces one half and keeps whatever the other already held. */
	constexpr uint16_t kLowByteMask = 0x00FF;
	constexpr uint16_t kHighByteMask = 0xFF00;
	constexpr int kHighByteShift = 8;

	/** ADPCM sample data is a stream of four-bit values, high nibble first. */
	constexpr uint8_t kNibbleMask = 0x0F;
	constexpr int kNibbleShift = 4;

	/** One bit per operator that reaches the output, in register order (0x30, 0x34, 0x38, 0x3C).
		Taken from the core's own per-algorithm early-out: algorithms 0-3 have a single carrier,
		4 has two, 5 and 6 have three, and 7 sums all four. */
	constexpr uint8_t kCarriers[8] = { 0x08, 0x08, 0x08, 0x08,
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
	uint32_t pow2Mask (std::size_t size)
	{
		if (size == 0)
		{
			return 0;
		}

		uint32_t mask = (uint32_t) (size - 1);
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
		int noteFor (uint32_t delta, int, int* finePitch) const override
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

		int noteFor (uint32_t fnum, int block, int* finePitch) const override
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

	constexpr uint32_t kAdpcmAGate = 3528;		// 80 ms at 44.1 kHz
	constexpr uint32_t kDacGate = 2205;			// 50 ms
	constexpr uint32_t kDacInterval = 2205;		// one DAC marker per 50 ms at most
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
		uint8_t totalLevel[4] = { 0x7F, 0x7F, 0x7F, 0x7F };

		/** 0x30-0x3F: the frequency multiplier of each operator, four per channel.

			Kept for the display rather than for the pitch: the channel's own f-number already says
			what the note is, and the multiplier only decides what the patch sounds like. Operator 1
			is the modulator on every algorithm but 7, where all four run in parallel - there the
			first operator is still the one whose ratio says the most about the patch. */
		int multiple[4] = { 1, 1, 1, 1 };

		/** 0xB0 bits 3-5: how hard the first operator feeds back into itself. */
		int feedback = 0;
	};

	/** One one-shot channel: the write starts it, its own end time stops it. `active` keeps the
		control bit's history so only a rising edge triggers. */
	struct Pulse
	{
		uint32_t end = 0;
		bool active = false;
	};

	/** ADPCM-B: the one sampled voice with a real pitch register (DELTA-N) and an end address.
		Everything the note needs is latched when the control register's start bit goes up, because
		the driver rewrites start/end/delta for the next sample before the next start.

		The control register's repeat bit (0x40) is not modelled. A looping sample is ended at its
		end address like a one-shot, so a sustained loop reports one note per start rather than one
		held note - which is what a MIDI note needs here anyway: the pitch comes from DELTA-N and
		does not change across the loop. Modelled when a set is found whose loops differ in pitch
		from their attack.
	*/
	struct AdpcmB
	{
		uint16_t startReg = 0;
		uint16_t endReg = 0;
		uint16_t delta = 0;
		uint8_t level = 0xFF;

		uint32_t latchStart = 0;
		uint32_t latchEnd = 0;
		bool active = false;
		uint32_t gateEnd = 0;
		double cycleLength = 0.0;
		bool melodic = false;
		uint32_t keyOns = 0;
	};

	/** **ADPCM-A is six sample players over one ROM**, each with its own start and end page. The
		pulse alone says a hit happened; these registers say which bytes it read, which is what a row
		has to draw. Port 1 offsets, from `fmopn.c`'s `FM_ADPCMAWrite`: 0x00 keys, 0x10+c/0x18+c start,
		0x20+c/0x28+c end, 0x08+c level. */
	struct AdpcmA
	{
		uint8_t startLow = 0;
		uint8_t startHigh = 0;
		uint8_t endLow = 0;
		uint8_t endHigh = 0;
		uint8_t level = 0x1F;
		uint32_t latchStart = 0;
		uint32_t latchEnd = 0;
	};

	/** The largest counts the tables below hold. A `Layout` must stay inside them. */
	static constexpr int kFmVoices = 6;
	static constexpr int kAdpcmAVoices = 6;

	/** **Where each voice family sits in the flat channel numbering the decoder reports.**

		One table instead of arithmetic. The boundary used to be computed - `fmCount + 6` for ADPCM-A,
		`fmCount + 6` again for the Delta-T row in `noteTable()` and `isDrum()` - which is only right
		while every variant with samples has exactly six FM voices. A variant with three would have
		read past `adpcmARegs[6]` and pointed the Delta-T row at somebody else's channel; nothing
		divides those families today except this table, so they are stated once, here.
	*/
	struct Layout
	{
		int fm = 0;
		int adpcmA = 0;
		bool adpcmB = false;
		bool dac = false;			// the YM2612's sixth channel is a DAC, not a voice

		int count (void) const { return fm + adpcmA + (adpcmB ? 1 : 0) + (dac ? 1 : 0); }
		/** The one Delta-T channel, or -1 when the variant has none. */
		int adpcmBChannel (void) const { return adpcmB ? fm + adpcmA : -1; }
		bool hasAdpcm (void) const { return adpcmA > 0 || adpcmB; }
	};

	Variant variant = Variant::ym2612;
	Layout layout;
	Channel channels[kFmVoices];
	Pulse adpcmA[kAdpcmAVoices];
	AdpcmA adpcmARegs[kAdpcmAVoices];
	std::vector<uint8_t> adpcmARom;
	uint32_t adpcmAMask = 0;
	AdpcmB adpcmB;
	Pulse dac;
	uint32_t lastDacTrigger = 0;
	bool dacTriggered = false;
	bool dacEnabled = false;
	uint32_t now = 0;
	double clock = 8000000.0;
	double streamRate = 44100.0;
	std::vector<uint8_t> adpcmBRom;
	uint32_t adpcmBMask = 0;
	std::map<uint32_t, double> adpcmBCycleCache;

	/** **The decoded Delta-T shapes, keyed by the range they cover.**

		A shape is a function of three inputs and nothing else: `latchStart`, `latchEnd` and the ROM
		bytes. The first two are latched at key-on and stay put until the next one, and the bytes only
		change in `setAdpcmBRom()` - so for a given range the shape is an **invariant**, which is the
		only reason caching it is sound rather than a guess.

		It has to be cached because `channelState()` is not a per-frame call: the note extractor asks
		once for **every register write**, and the snapshot asks for every channel of every block. On
		`07 Stage 2` that decoded the same kilobytes tens of thousands of times - 3.7 seconds of loading
		instead of 0.15. Cost here is per-call cost times call count, and the count is the amplifier.

		Two invalidations, both cheap: `setAdpcmBRom()` clears it (the bytes the shapes came from are
		gone), and 64 entries clear it wholesale - a song has a handful of samples, so exceeding that
		means something is wrong, and re-decoding one shape is cheaper than keeping an LRU honest.
	*/
	std::map<uint64_t, std::shared_ptr<const std::vector<float>>> adpcmBShapes;
	std::unique_ptr<OpnAdpcmBNoteTable> adpcmBTable;
	OpnNoteTable table;

	int channelCount = 7;			// derived from `layout` by configure(); never set by hand

	bool hasAdpcm (void) const { return layout.hasAdpcm (); }

	void configure (void)
	{
		switch (variant)
		{
		case Variant::ym2203:
			layout = Layout { 3, 0, false, false };
			break;

		case Variant::ym2608:
		case Variant::ym2610:
		case Variant::ym2610b:
			layout = Layout { 6, 6, true, false };
			break;

		case Variant::ym2612:
		default:
			layout = Layout { 6, 0, false, true };
			break;
		}

		// The tables hold these counts; a layout that outgrew them would be a silent overrun rather
		// than a compile error, so both halves are checked where the layout is chosen.
		assert (layout.fm <= kFmVoices);
		assert (layout.adpcmA <= kAdpcmAVoices);

		channelCount = layout.count ();

		if (variant == Variant::ym2610)
		{
			// **All six FM voices stay present.** The part is a YM2610 on paper - four FM voices,
			// with 1 and 4 unwired - but libvgm emulates the YM2610B for both ids, and the music in
			// these files was written for that one. Marking the two absent dropped their rows while
			// the mixer kept a fader for each, so every row below described the voice above it.
			channels[0].present = true;
			channels[3].present = true;
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

	uint32_t adpcmBAddress (uint16_t reg) const
	{
		return (uint32_t) reg << adpcmBShift();
	}

	uint8_t adpcmBByte (uint32_t address) const
	{
		const uint32_t index = address & adpcmBMask;
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
	double adpcmBUnitsPerSecond (uint16_t delta) const
	{
		return (double) delta * clock / (144.0 * 65536.0);
	}

	/** The sample's own cycle in nibbles, measured once per start address: the ADPCM-B ROM is
		4-bit delta data, so it is decoded first - the period only exists in the running sum. */
	double adpcmBCycleFor (uint32_t start)
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

		// `estimateCycleLength()` looks at 2048 samples - 1024 bytes - and truncates anything longer, so
		// decoding more than that only costs time: the answer is identical.
		constexpr std::size_t kBytes = 4096;
		std::vector<double> waveform;
		waveform.reserve (kBytes * 2);

		double accumulator = 0.0;
		int step = kAdpcmBStepMin;
		for (std::size_t i = 0; i < kBytes; ++i)
		{
			const int byte = adpcmBByte (start + (uint32_t) i);
			for (int half = 0; half < 2; ++half)
			{
				const int nibble = (half == 0) ? ((byte >> kNibbleShift) & kNibbleMask) : (byte & kNibbleMask);
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
		voice.active = true;
		++voice.keyOns;

		// DELTA-N is a pitch register, so every keyed voice with a non-zero DELTA-N is melodic -
		// the sample only decides whether a root correction can be added on top. Requiring a
		// measurable period made the whole channel one-shot whenever the set's ROM was absent or
		// its attack was noisy, which is what this used to do.
		voice.melodic = voice.delta != 0;

		// **Only a melodic voice reads the cycle.** Measuring it for a drum decodes a kilobyte and runs
		// a lag sweep whose result nothing consults - DELTA-N zero returns a one-shot note before the
		// number is ever read - and a Neo Geo track fires thousands of drums: on `07 Stage 2` that was
		// three seconds of load time for a value that was thrown away.
		voice.cycleLength = voice.melodic ? adpcmBCycleFor (voice.latchStart) : 0.0;

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
	void writeAdpcmB (uint8_t offset, uint8_t value)
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
			adpcmB.startReg = (uint16_t) ((adpcmB.startReg & kHighByteMask) | value);
			break;
		case 0x03:
			adpcmB.startReg = (uint16_t) ((adpcmB.startReg & kLowByteMask) | (value << kHighByteShift));
			break;
		case 0x04:
			adpcmB.endReg = (uint16_t) ((adpcmB.endReg & kHighByteMask) | value);
			break;
		case 0x05:
			adpcmB.endReg = (uint16_t) ((adpcmB.endReg & kLowByteMask) | (value << kHighByteShift));
			break;
		case 0x09:
			adpcmB.delta = (uint16_t) ((adpcmB.delta & kHighByteMask) | value);
			break;
		case 0x0A:
			adpcmB.delta = (uint16_t) ((adpcmB.delta & kLowByteMask) | (value << kHighByteShift));
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

	void keyEvent (uint8_t data)
	{
		int channel = (int) (data & kKeyChannelMask);
		if (channel == 3)
		{
			return;		// 0x28 with channel 3 is not a key register
		}

		if (layout.fm == 6 && (data & kKeyAllOperators) != 0)
		{
			channel += 3;
		}

		if (channel >= layout.fm || ! channels[channel].present)
		{
			return;
		}

		// Bits 4-7 are the operator mask; a zero mask is the key-off. A partial mask that does
		// not include a carrier is silent, which channelState() deals with.
		channels[channel].keyed = (data & kKeyOperatorMask) != 0;
	}

	void keyAdpcmA (uint32_t time, uint8_t data)
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

	void keyAdpcmB (uint8_t data)
	{
		// Bit 7 starts the sample and reloads the address, so a write with it on is a key-on even
		// when the voice was already sounding; bit 0 resets the unit.
		if ((data & 0x80) != 0)
		{
			startAdpcmB();
		}
		else if ((data & 0x01) != 0)
		{
			adpcmB.active = false;
		}
	}

	void dacByte (uint32_t time, uint8_t data)
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
		const uint8_t carriers = kCarriers[channel.algorithm & 0x07];
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
	[[maybe_unused]] const uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	const Variant variant = impl->variant;

	// the ROM belongs to the song, not to the clock
	std::vector<uint8_t> rom = std::move (impl->adpcmBRom);
	const uint32_t mask = impl->adpcmBMask;

	// **Which OPN this is arrives with the config.** The router keeps one decoder buffer per class,
	// so every OPN device was being built as the default YM2612: seven channels, no ADPCM rows at
	// all. A YM2610 song then showed six FM channels and a DAC where it should have shown six FM,
	// six ADPCM-A and one ADPCM-B. `flags` carries the variant as a small ordinal.
	const Variant requested = (config.flags >= 1 && config.flags <= 5)
								  ? (Variant) (config.flags - 1)
								  : variant;

	*impl = Impl();
	impl->variant = requested;
	impl->adpcmBRom = std::move (rom);
	impl->adpcmBMask = mask;
	impl->clock = clock > 0 ? (double) clock : 8000000.0;
	impl->streamRate = sampleRate > 0.0 ? sampleRate : 44100.0;
	impl->configure();
	impl->table.setClock ((double) clock);
}

void OpnDecoder::attachMemory (const ChipMemory& memory)
{
	if (memory.bytes == nullptr)
	{
		return;
	}

	// **A decoder that asked for more than one image may be handed either first.** The type says
	// which; zero means "the one the requirement names first", which is the ADPCM-A bank for every
	// caller that predates the Delta-T blocks.
	if (memory.type == kOpnbDeltaTBlock || memory.type == kOpnaDeltaTBlock)
	{
		setAdpcmBRom (*memory.bytes);
		return;
	}

	// The ADPCM-A image: one bank the six voices page through. The mask rounds to the power of two,
	// the way the core wraps an out-of-range address instead of reading zero.
	impl->adpcmAMask = pow2Mask (memory.bytes->size ());
	impl->adpcmARom = *memory.bytes;

	// **On the OPNA the two families share the part.** Delta-T reads the same ROM as the six voices
	// there, so a song that ships only `0x82` still has its Delta-T samples; the OPNB keeps them in a
	// block of its own, which arrives through attachSecondaryMemory() below.
	if (impl->variant == Variant::ym2608 && impl->adpcmBRom.empty ())
	{
		setAdpcmBRom (*memory.bytes);
	}
}

void OpnDecoder::attachSecondaryMemory (const ChipMemory& memory)
{
	// The Delta-T image, whatever type carried it (0x83 on the OPNB, 0x81 on the OPNA). Both end in
	// the same setter, so there is one rule for how it is masked and one cache to invalidate.
	if (memory.bytes != nullptr && ! memory.bytes->empty ())
	{
		setAdpcmBRom (*memory.bytes);
	}
}

void OpnDecoder::setAdpcmBRom (std::vector<uint8_t> rom)
{
	impl->adpcmBMask = pow2Mask (rom.size());
	impl->adpcmBRom = std::move (rom);
	impl->adpcmBCycleCache.clear();
	impl->adpcmBShapes.clear();		// the shapes were decoded from the bytes that just changed
}

void OpnDecoder::write (const RegisterWrite& write)
{
	const uint8_t reg = write.reg;
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
			const int slot = (int) (reg & 0x07);

			if (reg == 0x00)
			{
				// **The ranges are latched at the key-on**, because the driver rewrites start and
				// end for the next hit before the next start bit - the same reason ADPCM-B latches.
				const bool allOff = (write.value & 0x80) != 0;

				for (int channel = 0; channel < 6; ++channel)
				{
					if (allOff || (write.value & (1u << channel)) == 0)
					{
						continue;
					}

					Impl::AdpcmA& regs = impl->adpcmARegs[channel];
					regs.latchStart = (((uint32_t) regs.startHigh << 8) | regs.startLow) << 8;
					regs.latchEnd = ((((uint32_t) regs.endHigh << 8) | regs.endLow) << 8) + 0xFF;
				}

				impl->keyAdpcmA (write.sampleTime, write.value);
			}
			else if (reg >= 0x10 && reg <= 0x15)
			{
				impl->adpcmARegs[slot].startLow = (uint8_t) write.value;
			}
			else if (reg >= 0x18 && reg <= 0x1D)
			{
				impl->adpcmARegs[slot].startHigh = (uint8_t) write.value;
			}
			else if (reg >= 0x20 && reg <= 0x25)
			{
				impl->adpcmARegs[slot].endLow = (uint8_t) write.value;
			}
			else if (reg >= 0x28 && reg <= 0x2D)
			{
				impl->adpcmARegs[slot].endHigh = (uint8_t) write.value;
			}
			else if (reg >= 0x08 && reg <= 0x0D)
			{
				impl->adpcmARegs[slot].level = (uint8_t) (write.value & 0x1F);
			}

			return;
		}

		if (port == 0 && reg >= 0x10 && reg < 0x20)
		{
			// the OPNB's ADPCM-B block sits at 0x10 where the OPNA's sits at 0x00
			impl->writeAdpcmB ((uint8_t) (reg - 0x10), write.value);
			return;
		}
	}

	// Total level, one per operator: the only volume an FM channel has. In 0x40-0x4F the low
	// two bits select the channel inside the port's bank and bits 2-3 the operator, not the
	// other way round - swapping them sends each operator's level to a stranger's channel and
	// leaves the real carrier at the silent default (FM5 of a YM2612 song then played but had
	// no note: the channel's carriers still read 0x7F).
	// 0x30-0x3F: DT and MULTI, four operators per channel. Only the multiplier is kept - the
	// detune is a few cents and moves nothing a row can draw.
	if ((reg & 0xF0) == 0x30)
	{
		if ((reg & 0x03) == 3)
		{
			return;
		}

		const int channel = (int) (reg & 0x03) + port * 3;
		if (channel >= impl->layout.fm)
		{
			return;
		}

		impl->channels[channel].multiple[(reg >> 2) & 0x03] = (int) (write.value & 0x0F);
		return;
	}

	if ((reg & 0xF0) == 0x40)
	{
		if ((reg & 0x03) == 3)
		{
			return;		// 0xX3/0xX7/0xXB/0xXF: channel 3's special-mode slots, not a channel
		}

		const int channel = (int) (reg & 0x03) + port * 3;
		if (channel >= impl->layout.fm)
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
		if (channel >= impl->layout.fm)
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
		if (channel < impl->layout.fm)
		{
			Impl::Channel& state = impl->channels[channel];
			state.algorithm = (int) (write.value & 0x07);
			state.feedback = (int) ((write.value >> 3) & 0x07);
		}
	}
}

void OpnDecoder::advance (uint32_t sampleTime)
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
	if (channel < impl->layout.fm)
	{
		const Impl::Channel& voice = impl->channels[channel];
		if (! voice.present)
		{
			return state;
		}

		state.present = true;
		state.period = (uint32_t) voice.fnum;
		state.block = voice.block;

		const int level = impl->loudestCarrier (voice);
		const bool dacOwnsChannel = channel == 5 && impl->dacEnabled;
		state.gate = voice.keyed && level < 0x7F && ! dacOwnsChannel;
		state.volume = 15 - (level * 15) / 127;

		// The patch, for the display only. Operator 1's ratio and level, the channel's feedback,
		// and no waveform select - the OPN's operators have one sine each, unlike the OPM's four.
		// The total level is 0..127 here and 0..63 in the packed form (waveform::packFm), so it is
		// scaled rather than truncated.
		state.fmKnown = true;
		state.fmMultiple = voice.multiple[1];
		state.fmLevel = (voice.totalLevel[1] * 63) / 127;
		state.fmFeedback = voice.feedback;
		state.fmWaveform = 0;
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

	// **ADPCM-A is `layout.fm .. layout.fm + layout.adpcmA`**, and the Delta-T row is the one after
	// that range - both named by the same table, so a variant with a shorter FM section cannot shift
	// one family onto the other (a plain `fmCount + 6` did, and the Delta-T row then drew a
	// neighbour's sample). The slot is inside the table by construction and checked anyway: the
	// array is `kAdpcmAVoices` long.
	if (channel >= impl->layout.fm && channel < impl->layout.fm + impl->layout.adpcmA)
	{
		const std::size_t slot = (std::size_t) (channel - impl->layout.fm);

		if (slot >= (std::size_t) Impl::kAdpcmAVoices)
		{
			return state;
		}

		const Impl::AdpcmA& regs = impl->adpcmARegs[slot];

		state.period = kOneShotPeriod;
		state.volume = 12;
		state.gate = impl->now < impl->adpcmA[slot].end;

		// **The shape is the bytes the hit reads, decoded exactly as `fmopn.c` does.** A nibble
		// picks a delta out of `steps[]` scaled by the running step, and the nibble's low three bits
		// move the step by `step_inc`; the accumulator is **twelve bits and wraps** (unlike
		// Delta-T, which clamps), which is what the MSM5205 heritage buys. High nibble first.
		if (state.gate && ! impl->adpcmARom.empty () && regs.latchEnd > regs.latchStart)
		{
			static constexpr int stepTable[49] = {
				16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
				80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
				337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
				1282, 1411, 1552
			};
			static constexpr int stepInc[8] = { -16, -16, -16, -16, 32, 80, 112, 144 };
			constexpr uint32_t kWindow = 4096;

			const uint32_t length = (std::min) (regs.latchEnd - regs.latchStart, kWindow);
			auto wave = std::make_shared<std::vector<float>> ();
			wave->reserve (length * 2);

			int accumulator = 0;
			int step = 0;

			for (uint32_t i = 0; i < length; ++i)
			{
				const uint32_t index = (regs.latchStart + i) & impl->adpcmAMask;
				const uint8_t byte = (index < impl->adpcmARom.size ()) ? impl->adpcmARom[index] : 0;

				for (int half = 0; half < 2; ++half)
				{
					const int nibble = (half == 0) ? (byte >> 4) : (byte & 0x0F);
					const int delta = (2 * (nibble & 7) + 1) * stepTable[step / 16] / 8;

					accumulator = (accumulator + ((nibble & 8) != 0 ? -delta : delta)) & 0xFFF;
					step = std::max (0, std::min (48 * 16, step + stepInc[nibble & 7]));

					const int value = (accumulator & 0x800) != 0 ? (accumulator | ~0xFFF) : accumulator;
					wave->push_back ((float) value / 2048.0f);
				}
			}

			state.wave = wave;
		}

		// **One nibble every three DAC ticks.** The block has no pitch register, so the clock alone
		// sets the rate - and `clock / 144` is too fast by exactly three: the core's own key-on path
		// carries the figure as `(1<<ADPCM_SHIFT) * freqbase / 3.0` (the line it keeps commented out
		// because the value is precomputed), and 8 MHz / 432 = 18.5 kHz is the rate Neo Geo ADPCM-A
		// drums are recorded at. Reading them at 55.5 kHz made every drag-exported wave sound about
		// nineteen semitones sharp, which is what the ear reported.
		state.sampleRate = (float) (impl->clock / 432.0);
		return state;
	}

	// Everything below is the one Delta-T voice. A number the table does not give it is not a voice at
	// all, and an empty row is the honest answer - falling through used to make any unclaimed channel
	// report ADPCM-B's state.
	if (impl->layout.adpcmBChannel () != channel)
	{
		return state;
	}

	// ADPCM-B is the one sampled voice with a pitch register: DELTA-N is the note, the level
	// register the volume, and the start bit the gate
	const Impl::AdpcmB& voice = impl->adpcmB;
	state.period = std::max<uint32_t> (voice.delta, 1u);
	state.volume = (int) ((voice.level * 15u) / 255u);
	state.keyOns = voice.keyOns;
	// The level register is an offset, not a switch: the Neo Geo sets leave it at zero while the
	// channel plays loudly (0x1B is only written by drivers that attenuate), so gating on it
	// silenced every one of them. The start bit and the end address decide whether it sounds.
	state.gate = voice.active && (voice.gateEnd == 0 || impl->now < voice.gateEnd);

	// **The shape is the bytes the start and end registers name**, decoded with the same Delta-T table
	// the cycle estimate uses - clamped at 16 bits rather than wrapped, which is what separates Delta-T
	// from the six ADPCM-A voices. Drawn only while the voice is gated, like those rows.
	//
	// It is looked up rather than built, because this function is asked for a state far more often than
	// a person looks at a row: once per register write by the note extractor and once per channel per
	// block by the snapshot. The pair below covers every input the bytes depend on (a key-on latches
	// them, `setAdpcmBRom()` clears the cache), so a hit is the same shape, not an approximation.
	if (state.gate && ! impl->adpcmBRom.empty () && voice.latchEnd > voice.latchStart)
	{
		const uint64_t shapeKey = ((uint64_t) voice.latchStart << 32) | (uint64_t) voice.latchEnd;
		auto cached = impl->adpcmBShapes.find (shapeKey);

		if (cached == impl->adpcmBShapes.end ())
		{
			constexpr uint32_t kWindow = 4096;
			const uint32_t length = (std::min) (voice.latchEnd - voice.latchStart, kWindow);

			auto wave = std::make_shared<std::vector<float>> ();
			wave->reserve (length * 2);

			double accumulator = 0.0;
			int step = kAdpcmBStepMin;

			for (uint32_t i = 0; i < length; ++i)
			{
				const int byte = impl->adpcmBByte (voice.latchStart + i);

				for (int half = 0; half < 2; ++half)
				{
					const int nibble = (half == 0) ? ((byte >> kNibbleShift) & kNibbleMask)
												   : (byte & kNibbleMask);
					accumulator += (double) (kAdpcmBStepB1[nibble] * step / 8);
					accumulator = std::clamp (accumulator, -32768.0, 32767.0);
					wave->push_back ((float) (accumulator / 32768.0));
					step = std::clamp (step * kAdpcmBStepB2[nibble] / 64, kAdpcmBStepMin,
									   kAdpcmBStepMax);
				}
			}

			if (impl->adpcmBShapes.size () >= 64)
			{
				impl->adpcmBShapes.clear ();		// bounds a pathological song; costs one re-decode
			}

			cached = impl->adpcmBShapes.emplace (shapeKey, std::move (wave)).first;
		}

		state.wave = cached->second;

		// A sampled row states how fast it **plays**, not how fast it was recorded: DELTA-N is the rate
		// here, and the keyboard draws these bytes at that speed.
		state.sampleRate = (float) impl->adpcmBUnitsPerSecond (voice.delta);
	}

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
	if (impl->adpcmBTable != nullptr && channel == impl->layout.adpcmBChannel ())
	{
		return *impl->adpcmBTable;
	}

	return impl->table;
}

ChannelKind OpnDecoder::channelKind (int channel) const
{
	if (channel < impl->layout.fm)
	{
		return ChannelKind::fm;
	}

	return isDrum (channel) ? ChannelKind::adpcmDrum : ChannelKind::opnAdpcmB;
}

bool OpnDecoder::isDrum (int channel) const
{
	if (channel < impl->layout.fm)
	{
		return false;
	}

	// ADPCM-A is a trigger; ADPCM-B is melodic when its sample has a period
	return ! (channel == impl->layout.adpcmBChannel () && impl->adpcmB.melodic);
}

} // namespace toybox::chips
