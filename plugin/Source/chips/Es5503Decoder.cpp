#include "Es5503Decoder.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	// Above this a "period" is a measurement artifact, not a note - the same C8 ceiling the other
	// sampled players use.
	constexpr int kHighestMelodicNote = 108;

	// The window a table's cycle is measured in, and how far the scan walks. The same numbers the
	// GA20 decoder uses: 512 bytes keeps a scan local, and a table bigger than this is rare.
	constexpr uint32_t kMeasurementUnits = 512;
	constexpr uint32_t kScanStep = kMeasurementUnits / 2;
	constexpr uint32_t kScanBytes = 8 * 1024;
	constexpr uint32_t kMinimumWindow = 128;

	// A period is a lag where the waveform's correlation dips below this, and the period is the
	// peak that follows it - a steady tone anti-correlates at half its cycle, noise never dips.
	constexpr double kPeriodTrough = -0.5;

	/** An oscillator register block is 0x20 bytes: the low five bits of an offset are the
		oscillator, and 0xE0 upward are the chip-wide registers that sit past all thirty-two. */
	constexpr uint32_t kOscillatorIndexMask = 0x1F;

	/** The size register's low three bits index a table of powers of two, so the table length is
		256 << index in bytes - the value itself is not the length. */
	constexpr uint8_t kSizeIndexMask = 0x07;

	/** The control register packs three things: the halt bit in bit 0, the mode in bits 2-1, and
		the rest. A cleared halt bit on a register that had it set is the key-on edge, which is why
		the two tests below read the same bit on the old and the new value. */
	constexpr uint8_t kHaltBit = 0x01;
	constexpr int kModeShift = 1;
	constexpr uint8_t kModeMask = 0x03;

	/** 0xE1's operand: the number of oscillators the chip runs is `(value >> 1) + 1` - five bits
		plus one, because zero would be a chip that renders nothing. */
	constexpr int kOscillatorCountShift = 1;
	constexpr uint8_t kOscillatorCountMask = 0x1F;

	/** Sample data is unsigned bytes around a centre of 0x80; every other width in this project is
		signed, which is why the conversion is written out rather than cast. */
	constexpr int kSampleBias = 0x80;
	constexpr double kPeriodPeak = 0.5;

	// Table sizes and their pointer masks, straight out of es5503.c (`wavesizes` / `wavemasks`).
	// The mask is what keeps a table inside the chip's 128 KB however the driver wrote the pointer.
	constexpr uint32_t kTableSizes[8] = { 256, 512, 1024, 2048, 4096, 8192, 16384, 32768 };
	constexpr uint32_t kTableMasks[8] =
		{ 0x1ff00, 0x1fe00, 0x1fc00, 0x1f800, 0x1f000, 0x1e000, 0x1c000, 0x18000 };

	constexpr uint32_t kRamBytes = 128 * 1024;

	enum Mode
	{
		modeFree = 0,		// loops; only the control bit stops it
		modeOneShot = 1,	// one pass, then the core halts the oscillator
		modeSyncAm = 2,		// loops, and the pair syncs / modulates
		modeSwap = 3		// one pass, then the partner starts
	};

	/** The mask an address is folded with: the emulated RAM is a power-of-two buffer, so a table
		that runs past its end wraps instead of reading elsewhere. */
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
}

struct Es5503Decoder::Impl
{
	struct Oscillator
	{
		uint16_t frequency = 0;
		uint32_t tablePointer = 0;		// register byte << 8, plus bit 16 from the bank bit
		uint8_t volume = 0;
		uint8_t sizeIndex = 0;			// 0..7 -> 256..32768 bytes
		uint8_t resolution = 0;
		uint8_t control = 0;
		bool keyed = false;
		double cycleLength = 0.0;			// the table's own period in bytes, 0 = unmeasured
		bool melodic = false;
		uint32_t gateEnd = 0;			// 0 when the table loops
		uint32_t keyOns = 0;
	};

	Oscillator oscillators[Es5503Decoder::numChannels];
	std::vector<uint8_t> ram;
	uint32_t ramMask = 0;
	double clockRate = 7159090.0;			// the chip's master clock
	double outRate = 44100.0;				// the rate the note times and gates are in
	int enabledOscillators = 1;				// register 0xE1, 1..32
	std::map<uint32_t, double> cycleCache;
	uint32_t now = 0;

	uint8_t byteAt (uint32_t address) const
	{
		const uint32_t index = address & ramMask;
		return index < ram.size() ? ram[index] : 0;
	}

	/** The core's sample rate, `es5503_get_sample_rate`: the clock divided by eight and then by
		the number of enabled oscillators plus two. It is *not* a per-voice number, so a 0xE1 write
		moves every pitch on the chip - which is why the note tables read it live instead of
		latching it. */
	double chipRate (void) const
	{
		return (clockRate / 8.0) / (double) (enabledOscillators + 2);
	}

	/** Bytes a second this oscillator walks its table at: `freq` is added to the accumulator once
		per chip sample and the index is `accumulator >> (9 + resolution - size)`. */
	double entriesPerSecond (const Oscillator& oscillator) const
	{
		const double step = std::ldexp (1.0, 9 + (int) oscillator.resolution - (int) oscillator.sizeIndex);
		return (double) oscillator.frequency * chipRate() / step;
	}

	/** The best period in a window: the shortest lag whose correlation dips below `kPeriodTrough`
		and then peaks above `kPeriodPeak`, or 0 for a window with no such dip. */
	static double bestPeriod (const std::vector<double>& waveform)
	{
		const std::size_t size = waveform.size();
		if (size < 64)
		{
			return 0.0;
		}

		double mean = 0.0;
		for (double sample : waveform)
		{
			mean += sample;
		}

		mean /= (double) size;

		const std::size_t maxLag = std::min<std::size_t> (size / 2, 512);
		std::vector<double> correlation (maxLag + 1, 0.0);
		bool sawTrough = false;
		std::size_t troughLag = 0;
		double best = 0.0;

		for (std::size_t lag = 2; lag <= maxLag; ++lag)
		{
			double product = 0.0;
			double energyA = 0.0;
			double energyB = 0.0;
			for (std::size_t i = 0; i + lag < size; ++i)
			{
				const double a = waveform[i] - mean;
				const double b = waveform[i + lag] - mean;
				product += a * b;
				energyA += a * a;
				energyB += b * b;
			}

			if (energyA <= 0.0 || energyB <= 0.0)
			{
				continue;
			}

			correlation[lag] = product / std::sqrt (energyA * energyB);
			if (! sawTrough)
			{
				sawTrough = correlation[lag] <= kPeriodTrough;
				troughLag = lag;
			}
			else if (correlation[lag] > best)
			{
				best = correlation[lag];
			}
		}

		if (! sawTrough || best < kPeriodPeak)
		{
			return 0.0;
		}

		// From the trough, not from the start: a smooth table correlates with itself at lag 2 or 3
		// by mere continuity, and returning that reports every tone an octave or two high. And a
		// *local maximum*, not just "close to the best": the correlation of a square wave climbs
		// linearly to the period, so a lag a few percent short of it is already within a tenth of
		// the peak and would be read as a slightly sharp note.
		for (std::size_t lag = troughLag + 1; lag <= maxLag; ++lag)
		{
			// the last lag has no neighbour to fall towards, and it is where a cycle of exactly
			// half the window peaks - the case a 1024-byte table with four cycles lands on
			const bool peak = correlation[lag] >= correlation[lag - 1]
							  && (lag == maxLag || correlation[lag] >= correlation[lag + 1]);
			if (peak && correlation[lag] >= best * 0.9 && correlation[lag] >= kPeriodPeak)
			{
				return (double) lag;
			}
		}

		return 0.0;
	}

	/** The table's own cycle in bytes, measured once per (pointer, size).

		A DOC table is a *waveform*: one cycle in the Apple IIgs tradition, or several when the
		driver wants a brighter sound - the stage theme of Undercover Cops uploads 1024 bytes
		holding ten cycles of a 100-byte wave, and reading the table as one cycle would put every
		note three octaves low. So the cycle is measured, exactly as a sample's is in the GA20
		decoder: 512-byte windows from the start, first one with a period wins.

		A single-cycle table cannot show its period in a window of its own length (there is no
		repeat inside it), so the measurement returns 0 there; the caller falls back to the table
		length when the mode loops, because that *is* the rate the chip repeats the table at. */
	double measuredCycle (uint32_t base, int sizeIndex)
	{
		const uint32_t key = (base << 3) | (uint32_t) sizeIndex;
		const auto cached = cycleCache.find (key);
		if (cached != cycleCache.end())
		{
			return cached->second;
		}

		double cycle = 0.0;
		if (! ram.empty())
		{
			const uint32_t length = kTableSizes[sizeIndex];
			const uint32_t limit = std::min<uint32_t> (length, kScanBytes);
			for (uint32_t offset = 0; cycle <= 0.0 && offset + kMinimumWindow <= limit;
				 offset += kScanStep)
			{
				const uint32_t window =
					std::min<uint32_t> (limit - offset, kMeasurementUnits);
				std::vector<double> waveform (window);
				for (uint32_t i = 0; i < window; ++i)
				{
					waveform[i] = (double) ((int) byteAt (base + offset + i) - kSampleBias);
				}

				cycle = bestPeriod (waveform);
			}
		}

		cycleCache[key] = cycle;
		return cycle;
	}

	uint32_t tableBase (const Oscillator& oscillator) const
	{
		const int index = oscillator.sizeIndex & kSizeIndexMask;
		return oscillator.tablePointer & kTableMasks[index];
	}

	void keyOn (Oscillator& oscillator, uint32_t time)
	{
		++oscillator.keyOns;

		const int sizeIndex = oscillator.sizeIndex & kSizeIndexMask;
		const uint32_t base = tableBase (oscillator);
		const uint32_t length = kTableSizes[sizeIndex];
		const int mode = (oscillator.control >> kModeShift) & kModeMask;

		double cycle = measuredCycle (base, sizeIndex);
		if (cycle <= 0.0 && (mode == modeFree || mode == modeSyncAm))
		{
			cycle = (double) length;		// the table repeats at its own length
		}

		oscillator.cycleLength = cycle;
		oscillator.melodic = false;
		oscillator.gateEnd = 0;

		const double rate = entriesPerSecond (oscillator);
		if (cycle > 0.0 && rate > 0.0)
		{
			int fine = 0;
			const int note = quantiseFrequency (rate / cycle, &fine);
			oscillator.melodic = note >= 0 && note <= kHighestMelodicNote;
		}

		// a mode that does not loop ends when the table runs out - that length is exact
		if ((mode == modeOneShot || mode == modeSwap) && rate > 0.0)
		{
			oscillator.gateEnd = time + (uint32_t) std::llround ((double) length * outRate / rate);
		}

		oscillator.keyed = true;
	}

	/** Control bit 0 is the halt bit, so a note starts when it falls and ends when it rises.
		Writing the same clear value again does not retrigger - the core only resets the
		accumulator when the bit was set - and a one-shot that ran out counts as set, because the
		core sets it itself (`es5503_halt_osc`). */
	void writeControl (Oscillator& oscillator, uint8_t value, uint32_t time)
	{
		const bool wasHalted = (oscillator.control & kHaltBit) != 0;
		oscillator.control = value;

		if (wasHalted && (value & kHaltBit) == 0)
		{
			keyOn (oscillator, time);
			return;
		}

		if ((value & kHaltBit) != 0)
		{
			oscillator.keyed = false;		// the halt bit: the note ends here
			oscillator.gateEnd = 0;
		}
	}

	int volumeOf (const Oscillator& oscillator) const
	{
		// 8-bit linear, and the core allows more than 127 - the extractor carries 0..127
		return std::min (127, ((int) oscillator.volume + 1) / 2);
	}

	bool sounding (const Oscillator& oscillator) const
	{
		if (! oscillator.keyed || oscillator.volume == 0)
		{
			return false;
		}

		return oscillator.gateEnd == 0 || now < oscillator.gateEnd;
	}

	/** One table per oscillator: the note is `entriesPerSecond / cycle`, and both of those live on
		the oscillator's state (the resolution and size registers are part of the step), so the
		table reads through to it rather than caching a number that a later write would stale. */
	class Note final : public NoteTable
	{
	public:
		Note (const Impl& owner, int index)
			: impl (owner)
			, channel (index) {}

		int noteFor (uint32_t frequency, int, int* finePitch) const override
		{
			*finePitch = 0;

			const Oscillator& oscillator = impl.oscillators[channel];
			if (frequency == 0 || ! oscillator.melodic || oscillator.cycleLength <= 0.0)
			{
				return 60;		// a drum (or a silent table): the C4 trigger
			}

			// No L1: this register is a *step through a table*, not a playback rate, so there is no
			// reference rate to measure a transposition against - one entry per output sample is
			// 25 Hz and ten times that is 250 Hz, all of it a property of the table, not of a
			// sample. The measured table cycle is therefore the whole pitch, and the note reports
			// it as an inference (design doc section 14.3, B class).
			return quantiseFrequency (impl.entriesPerSecond (oscillator) / oscillator.cycleLength,
									  finePitch);
		}

	private:
		const Impl& impl;
		int channel;
	};

	std::unique_ptr<Note> tables[Es5503Decoder::numChannels];
};

Es5503Decoder::Es5503Decoder()
	: impl (std::make_unique<Impl>())
{
}

Es5503Decoder::~Es5503Decoder() = default;

void Es5503Decoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	// the RAM belongs to the song, not to the run
	std::vector<uint8_t> ram = std::move (impl->ram);
	const uint32_t mask = impl->ramMask;

	*impl = Impl();
	impl->ram = std::move (ram);
	impl->ramMask = mask;
	impl->outRate = sampleRate > 0.0 ? sampleRate : 44100.0;
	impl->clockRate = (clock > 0) ? (double) clock : 7159090.0;

	// Every oscillator starts halted. The core's reset leaves the halt bit clear but clears the
	// RAM to 0x00, and the first update reads that byte and halts the oscillator itself - so by
	// the time a driver writes its first control byte, bit 0 is set and clearing it is the key-on.
	// Leaving it clear here lost every voice whose first control write is 0x00 (the DOC driver of
	// Undercover Cops does exactly that), and those voices then played on the chip and stood
	// silent in MIDI.
	for (auto& oscillator : impl->oscillators)
	{
		oscillator.control = 0x01;
	}

	for (int i = 0; i < numChannels; ++i)
	{
		impl->tables[i] = std::make_unique<Impl::Note> (*impl, i);
	}
}

void Es5503Decoder::setRam (std::vector<uint8_t> ram)
{
	impl->ramMask = pow2Mask (ram.size());
	impl->ram = std::move (ram);
	impl->cycleCache.clear();
}

void Es5503Decoder::write (const RegisterWrite& write)
{
	const uint8_t offset = write.reg;

	if (offset >= 0xE0)
	{
		if (offset == 0xE1)
		{
			// `(value >> 1) + 1` oscillators; the rate the whole chip runs at changes with it
			impl->enabledOscillators = (int) ((write.value >> kOscillatorCountShift) & kOscillatorCountMask) + 1;
		}

		return;		// 0xE0 / 0xE2 are read-only
	}

	const int channel = offset & kOscillatorIndexMask;
	Impl::Oscillator& oscillator = impl->oscillators[channel];

	switch (offset & 0xE0)
	{
	case 0x00:		// frequency, low byte
		oscillator.frequency = (uint16_t) ((oscillator.frequency & 0xFF00) | write.value);
		break;

	case 0x20:		// frequency, high byte
		oscillator.frequency = (uint16_t) ((oscillator.frequency & 0x00FF)
												| ((uint16_t) write.value << 8));
		break;

	case 0x40:		// volume
		oscillator.volume = write.value;
		break;

	case 0x60:		// the current sample byte, read-only
		break;

	case 0x80:		// wavetable pointer, high byte only
		oscillator.tablePointer = (oscillator.tablePointer & 0x10000u) | ((uint32_t) write.value << 8);
		break;

	case 0xA0:		// control: halt bit, mode, IRQ, output channel
		if (oscillator.gateEnd != 0 && impl->now >= oscillator.gateEnd)
		{
			oscillator.control |= 0x01;		// the core halted this one-shot itself
			oscillator.keyed = false;
		}

		impl->writeControl (oscillator, write.value, write.sampleTime);
		break;

	case 0xC0:		// bank bit, table size, resolution
		oscillator.tablePointer = (write.value & 0x40)
									  ? (oscillator.tablePointer | 0x10000u)
									  : (oscillator.tablePointer & 0xFFFFu);
		oscillator.sizeIndex = (uint8_t) ((write.value >> 3) & 0x07);
		oscillator.resolution = (uint8_t) (write.value & 0x07);
		break;

	default:
		break;
	}
}

void Es5503Decoder::advance (uint32_t sampleTime)
{
	impl->now = sampleTime;
}

ChannelState Es5503Decoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= numChannels)
	{
		return state;
	}

	const Impl::Oscillator& oscillator = impl->oscillators[channel];
	state.present = true;
	state.gate = impl->sounding (oscillator);
	state.keyOns = oscillator.keyOns;
	state.period = oscillator.frequency;
	state.volume = impl->volumeOf (oscillator);

	// B class: the note already carries the whole pitch (the table's measured cycle), so there is
	// no separate root correction to add - and no transposition either
	state.hz = (oscillator.cycleLength > 0.0 && oscillator.melodic)
				   ? impl->entriesPerSecond (oscillator) / oscillator.cycleLength
				   : 0.0;
	state.transposeCents = 0;
	state.rootCents = 0;
	state.pitchSource = oscillator.melodic ? PitchSource::sampleInferred : PitchSource::unpitched;

	return state;
}

const NoteTable& Es5503Decoder::noteTable (int channel) const
{
	const int index = (channel >= 0 && channel < numChannels) ? channel : 0;
	return *impl->tables[index];
}

ChannelKind Es5503Decoder::channelKind (int channel) const
{
	return isDrum (channel) ? ChannelKind::adpcmDrum : ChannelKind::wavetableWave;
}

bool Es5503Decoder::isDrum (int channel) const
{
	if (channel < 0 || channel >= numChannels)
	{
		return true;
	}

	return ! impl->oscillators[channel].melodic;
}

} // namespace toybox::chips
