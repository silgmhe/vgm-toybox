#include "K053260Decoder.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <vector>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	// Konami's boards clock the K053260 at 3.579545 MHz; the header supplies the real one.
	constexpr std::uint32_t kDefaultClock = 3579545;

	// k053260.c: `CLOCKS_PER_SAMPLE = 64`, so the chip renders at `clock / 64` (55.9 kHz here).
	constexpr double kOutputDivisor = 64.0;

	// One output sample advances the voice by `pitch / 4096` bytes (`KDSC_play`: a 12-bit
	// accumulator that overflows at 0x1000), so the byte rate is pitch * clock / (64 * 4096).
	// A third of the C140's and C352's divisor, but the same kind of number: a rate, not a note.
	constexpr double kPitchDivisor = 64.0 * 4096.0;

	constexpr std::uint8_t kKeyMaskOffset = 0x28;
	constexpr std::uint8_t kLoopModeOffset = 0x2A;
	constexpr std::uint8_t kPanLowOffset = 0x2C;
	constexpr std::uint8_t kPanHighOffset = 0x2D;
	constexpr std::uint8_t kControlOffset = 0x2F;
	constexpr std::uint8_t kControlSoundEnable = 0x02;

	// KADPCM's sixteen deltas (`kadpcm_table` in k053260.c): a nibble is added to a running sum,
	// which is why the format has to be decoded before the waveform can be measured.
	constexpr int kKadpcmTable[16] = { 0, 1, 2, 4, 8, 16, 32, 64, -128, -64, -32, -16, -8, -4, -2, -1 };

	/** The mask the core folds a sample address with (`pow2_mask` in EmuHelper.h): the ROM is
		covered as a power-of-two buffer, so a sample that runs off the end wraps instead of reading
		elsewhere. Measured addresses are masked the same way. */
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

	// Above this the "pitch" is a measurement artifact, not a note: a drum whose bytes repeat every
	// eight samples reads as 6 kHz at this chip's rates, and no music on these boards is written up
	// there. C8 is the ceiling, so anything above it is treated as the percussion it is.
	constexpr int kHighestMelodicNote = 108;

	/** One note table per voice: the pitch register is a rate, and the sample each voice plays has
		its own cycle length. `cycleLength` is the voice's latched estimate and `melodic` says whether
		it came out as a usable pitch - a sample that does not repeat, or repeats at a rate no note is
		written at, is a drum. */
	class K053260NoteTable final : public NoteTable
	{
	public:
		K053260NoteTable (double clock, const double* sampleCycle, const bool* melodic,
						  const std::vector<std::uint8_t>* rom)
			: bytesPerSecondPerStep (clock / kPitchDivisor)
			, rootRate (clock / kOutputDivisor)
			, cycleLength (sampleCycle)
			, melodic (melodic)
			, rom (rom) {}

		int noteFor (std::uint32_t period, int, int* finePitch) const override
		{
			const double rate = (double) period * bytesPerSecondPerStep;

			if (melodic != nullptr && *melodic && cycleLength != nullptr && *cycleLength > 0.0)
			{
				// L1: the transposition the pitch register asks for, never the sample's bytes
				return quantisePlaybackRate (rate, rootRate, finePitch);
			}

			if (rom == nullptr || rom->empty())
			{
				// no ROM, nothing to measure: the register against the chip's own rate, with the
				// top of the pitch register (one byte per output sample) as middle C
				return quantisePlaybackRate (rate, rootRate, finePitch);
			}

			// a drum: report the one-shot trigger, not a pitch
			if (finePitch != nullptr)
			{
				*finePitch = 0;
			}

			return 60;
		}

	private:
		double bytesPerSecondPerStep;
		double rootRate;
		const double* cycleLength;
		const bool* melodic;
		const std::vector<std::uint8_t>* rom;
	};

	/** What a measurement belongs to: the registers that name one sample's bytes. */
	struct SampleKey
	{
		std::uint32_t start;
		std::uint16_t length;
		bool kadpcm;
		bool reverse;

		bool operator< (const SampleKey& other) const
		{
			return std::tie (start, length, kadpcm, reverse)
				   < std::tie (other.start, other.length, other.kadpcm, other.reverse);
		}
	};
}

struct K053260Decoder::Impl
{
	struct Voice
	{
		std::uint16_t pitch = 0;		// 12 bits
		std::uint16_t length = 0;		// bytes
		std::uint32_t start = 0;		// 21 bits
		std::uint8_t volume = 0;		// 7 bits
		std::uint8_t pan = 0;			// 3 bits, 0 = silent, 4 = centre
		bool loop = false;
		bool kadpcm = false;
		bool reverse = false;

		// latched where the core latches its pointer and counter
		bool keyed = false;
		std::uint32_t keyOns = 0;		// monotonic: a retrigger at the same pitch is a new note
		std::uint32_t gateEnd = 0;
		double cycleLength = 0.0;		// one cycle in fetch units; 0 = no period (a drum)
		bool melodic = false;			// cycleLength turned into a note this chip could be playing
	};

	Voice voices[numChannels];
	std::uint32_t now = 0;
	double clock = (double) kDefaultClock;
	double streamRate = 44100.0;
	std::vector<std::uint8_t> rom;
	std::uint32_t romMask = 0;
	std::uint8_t keyMask = 0;			// the level 0x28 last held, for the rising edges
	std::uint8_t mode = 0;				// 0x2F: bit 1 gates all sound, as in the core
	std::map<SampleKey, double> cycleCache;
	std::vector<std::unique_ptr<K053260NoteTable>> tables;

	/** How many fetch units a sample holds: one byte each for PCM, two nibbles each for KADPCM. */
	double fetchUnits (const Voice& voice) const
	{
		return (voice.kadpcm ? 2.0 : 1.0) * (double) voice.length;
	}

	std::uint8_t romByte (std::uint32_t address) const
	{
		const std::uint32_t index = address & romMask;
		return (index < rom.size()) ? rom[index] : 0;
	}

	/** How long a non-looping voice lasts, in stream samples.

		The core stops when the pointer runs past the length register, so the length is exact - no
		gate guess, unlike the RF5C68. A loop restarts the sample instead, so only a key-off ends
		it. A zero pitch never overflows the 12-bit accumulator and a zero length is nothing to
		fetch: both are silence, exactly as in the core. */
	std::uint32_t durationOf (const Voice& voice) const
	{
		if (voice.loop || voice.length == 0 || voice.pitch == 0)
		{
			return 0;
		}

		const double perSecond = (double) voice.pitch * clock / kPitchDivisor;
		const double samples = fetchUnits (voice) / perSecond * streamRate;
		return (std::uint32_t) std::llround (std::min (samples, 1.0e9));
	}

	/** The cycle length of the sample a voice is about to play, measured once per sample.

		The core's first fetch is the byte *after* the start address (`KDSC_play` pre-increments;
		its comment names TMNT2 and Vendetta), so the region measured here begins at `start + 1` -
		one byte early would put a DC step in front of every KADPCM sample. A loop wraps to the
		start, but that only shifts the phase of a repeating waveform, not its period.

		KADPCM is integrated first: the nibbles are deltas, and only the summed waveform has the
		period the sample was recorded with. A voice that loops has no separate loop address, so
		the whole region is the repeating part either way. */
	double cycleFor (const Voice& voice)
	{
		if (rom.empty() || voice.length == 0)
		{
			return 0.0;
		}

		const SampleKey key { voice.start, voice.length, voice.kadpcm, voice.reverse };
		const auto cached = cycleCache.find (key);
		if (cached != cycleCache.end())
		{
			return cached->second;
		}

		const std::uint32_t begin = voice.start + 1;
		std::vector<double> waveform;

		if (! voice.kadpcm)
		{
			waveform.resize (voice.length);
			for (std::size_t i = 0; i < waveform.size(); ++i)
			{
				waveform[i] = (double) (std::int8_t) romByte (begin + (std::uint32_t) i);
			}
		}
		else
		{
			// low nibble first: the core shifts the byte down when `position` is odd, and the
			// first nibble it reads is the low one
			waveform.resize ((std::size_t) fetchUnits (voice));
			double accumulator = 0.0;
			for (std::size_t i = 0; i < waveform.size(); ++i)
			{
				const std::uint8_t byte = romByte (begin + (std::uint32_t) (i / 2));
				const int nibble = (i % 2 == 0) ? (byte & 0x0F) : (byte >> 4);
				accumulator += (double) kKadpcmTable[nibble];
				waveform[i] = accumulator;
			}
		}

		const double cycle = estimateCycleLength (waveform, 0, 0);
		cycleCache[key] = cycle;
		return cycle;
	}

	/** The key-on edge of one voice: the core copies the pointer, forces the counter and reads the
		registers that are live at that moment (length, pitch, format), so the note's length and its
		pitch both come from here. */
	void keyOn (int channel, std::uint32_t sampleTime)
	{
		Voice& voice = voices[channel];
		voice.keyed = true;
		++voice.keyOns;
		voice.gateEnd = sampleTime + durationOf (voice);
		voice.cycleLength = cycleFor (voice);

		// A period only becomes a note when the rate turns it into a pitch somebody wrote. A drum
		// often correlates at a handful of samples - an alternating byte pair repeats every two -
		// and at this chip's rates that lands in the kilohertz, so the ceiling is what separates a
		// measured note from a measurement artifact.
		voice.melodic = false;
		if (! rom.empty() && voice.cycleLength > 0.0)
		{
			const double rate = (double) voice.pitch * clock / kPitchDivisor;
			int fine = 0;
			voice.melodic = quantiseFrequency (rate / voice.cycleLength, &fine) <= kHighestMelodicNote;
		}
	}

	int volumeOf (const Voice& voice) const
	{
		if (voice.pan == 0)
		{
			return 0;		// pan_mul[0] is { 0, 0 }: panned fully out is silence
		}

		return std::min (15, ((int) voice.volume * 15 + 63) / 127);
	}

	bool isSounding (int channel) const
	{
		const Voice& voice = voices[channel];

		// k053260_update returns silence unless the control register enables the output, and a
		// voice with no pitch or no length never fetches a byte
		if (! voice.keyed || (mode & kControlSoundEnable) == 0)
		{
			return false;
		}

		if (voice.pitch == 0 || voice.length == 0 || volumeOf (voice) == 0)
		{
			return false;
		}

		return voice.loop || now < voice.gateEnd;
	}
};

K053260Decoder::K053260Decoder() : impl (std::make_unique<Impl>())
{
	reset (kDefaultClock, 44100.0);
}

K053260Decoder::~K053260Decoder() = default;

void K053260Decoder::reset (std::uint32_t clock, double sampleRate)
{
	// the ROM belongs to the song, not to the clock: a reset only changes the rate the chip runs at
	std::vector<std::uint8_t> rom = impl != nullptr ? std::move (impl->rom) : std::vector<std::uint8_t>();
	const std::uint32_t romMask = impl != nullptr ? impl->romMask : 0u;

	impl = std::make_unique<Impl>();
	impl->clock = (clock > 100000) ? (double) clock : (double) kDefaultClock;
	impl->streamRate = (sampleRate > 0.0) ? sampleRate : 44100.0;
	impl->rom = std::move (rom);
	impl->romMask = romMask;
	impl->tables.reserve ((std::size_t) numChannels);
	for (auto& voice : impl->voices)
	{
		impl->tables.push_back (std::make_unique<K053260NoteTable> (impl->clock, &voice.cycleLength,
																   &voice.melodic, &impl->rom));
	}
}

void K053260Decoder::setRom (std::vector<std::uint8_t> rom)
{
	impl->romMask = pow2Mask (rom.size());
	impl->rom = std::move (rom);
	impl->cycleCache.clear();		// the samples describe a different ROM now
}

void K053260Decoder::write (const RegisterWrite& w)
{
	const std::uint8_t offset = (std::uint8_t) (w.reg & 0x3F);
	const std::uint8_t value = w.value;

	if (offset >= 0x08 && offset <= 0x27)
	{
		const int channel = (int) ((offset - 0x08) / 8);
		Impl::Voice& voice = impl->voices[channel];

		switch (offset & 7)
		{
		case 0:		// pitch, low eight bits
			voice.pitch = (std::uint16_t) ((voice.pitch & 0x0F00) | value);
			break;
		case 1:		// pitch, high four bits
			voice.pitch = (std::uint16_t) ((voice.pitch & 0x00FF) | ((value << 8) & 0x0F00));
			break;
		case 2:		// length, low eight bits
			voice.length = (std::uint16_t) ((voice.length & 0xFF00) | value);
			break;
		case 3:		// length, high eight bits
			voice.length = (std::uint16_t) ((voice.length & 0x00FF) | (value << 8));
			break;
		case 4:		// start, low eight bits
			voice.start = (voice.start & 0x1FFF00u) | value;
			break;
		case 5:		// start, middle eight bits
			voice.start = (voice.start & 0x1F00FFu) | ((std::uint32_t) value << 8);
			break;
		case 6:		// start, high five bits
			voice.start = (voice.start & 0x00FFFFu) | (((std::uint32_t) value << 16) & 0x1F0000u);
			break;
		case 7:		// volume, seven bits
			voice.volume = (std::uint8_t) (value & 0x7F);
			break;
		default:
			break;
		}
		return;
	}

	switch (offset)
	{
	case kKeyMaskOffset:
	{
		// the whole key-on/key-off protocol lives here: a rising bit starts a voice (the core
		// compares against the level it last saw), a cleared bit stops it, and the high nibble is
		// that voice's reverse flag
		const std::uint8_t rising = (std::uint8_t) (value & ~impl->keyMask);

		for (int channel = 0; channel < channelCount_; ++channel)
		{
			Impl::Voice& voice = impl->voices[channel];
			voice.reverse = ((value >> (channel + 4)) & 1) != 0;

			if ((rising & (1 << channel)) != 0)
			{
				impl->keyOn (channel, w.sampleTime);
			}
			else if ((value & (1 << channel)) == 0)
			{
				voice.keyed = false;
				voice.gateEnd = 0;
			}
		}

		impl->keyMask = value;
		break;
	}
	case kLoopModeOffset:
		for (int channel = 0; channel < channelCount_; ++channel)
		{
			impl->voices[channel].loop = ((value >> channel) & 1) != 0;
			impl->voices[channel].kadpcm = ((value >> (channel + 4)) & 1) != 0;
		}
		break;
	case kPanLowOffset:
		impl->voices[0].pan = (std::uint8_t) (value & 0x07);
		impl->voices[1].pan = (std::uint8_t) ((value >> 3) & 0x07);
		break;
	case kPanHighOffset:
		impl->voices[2].pan = (std::uint8_t) (value & 0x07);
		impl->voices[3].pan = (std::uint8_t) ((value >> 3) & 0x07);
		break;
	case kControlOffset:
		impl->mode = value;
		break;
	default:
		break;		// the ports and the read-only registers carry no note
	}
}

void K053260Decoder::advance (std::uint32_t sampleTime)
{
	impl->now = sampleTime;

	for (auto& voice : impl->voices)
	{
		// the core clears `playing` when a non-looping sample runs past its length
		if (voice.keyed && ! voice.loop && sampleTime >= voice.gateEnd)
		{
			voice.keyed = false;
		}
	}
}

ChannelState K053260Decoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= channelCount_)
	{
		return state;
	}

	const Impl::Voice& voice = impl->voices[channel];
	state.present = true;
	state.gate = impl->isSounding (channel);
	state.keyOns = voice.keyOns;
	state.period = voice.pitch;
	state.volume = impl->volumeOf (voice);

	const SampledPitch pitch = describeSamplePitch ((double) voice.pitch * impl->clock / kPitchDivisor,
													impl->clock / kOutputDivisor, voice.cycleLength,
													voice.melodic);
	state.hz = pitch.hz;
	state.transposeCents = pitch.transposeCents;
	state.rootCents = pitch.rootCents;
	state.pitchSource = pitch.source;

	return state;
}

const NoteTable& K053260Decoder::noteTable (int channel) const
{
	const std::size_t index = (channel >= 0 && channel < channelCount_) ? (std::size_t) channel : 0;
	return *impl->tables[index];
}

Family K053260Decoder::family (int channel) const
{
	return isDrum (channel) ? Family::adpcmDrum : Family::k053260Pcm;
}

bool K053260Decoder::isDrum (int channel) const
{
	if (channel < 0 || channel >= channelCount_ || impl->rom.empty())
	{
		return false;
	}

	// a sample that does not repeat, or that "repeats" at a rate above anything written for these
	// boards, has no pitch to report: it is a drum hit, and the note table turns it into the C4
	// trigger
	return ! impl->voices[channel].melodic;
}

} // namespace toybox::chips
