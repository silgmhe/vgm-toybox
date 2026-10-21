#include "C352Decoder.h"

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
	// System 22 / System 23 boards run the C352 at 12.288 MHz (42.67 kHz at clock / 288); the
	// header clock is used whenever it is there.
	constexpr std::uint32_t kDefaultClock = 12288000;

	// c352.c: the voice counter is 16.16 and gains `frequency` every output sample, and the output
	// runs at `clock / 288`. Dividing by 288 * 65536 turns the register and the clock into bytes a
	// second - the same divisor the C140 uses, because both chips advance a sample pointer the same
	// way.
	constexpr double kClockDivisor = 18874368.0;

	// The rate a C352 sample ROM is taken to play at its recorded pitch when its own cycle cannot
	// be measured: the chip's own 8-bit rate, `clock / (288 * 2)` (c352.c's header says "output
	// sample rate is the input clock / (288 * 2)"). A frequency register at 0x8000 - halfway up,
	// where a driver's mid-range notes sit - plays a sample at exactly this rate and sounds middle
	// C; the top of the register is an octave above. The root follows the clock rather than being a
	// fixed number, and it is an argument of quantisePlaybackRate() so the settings can override it.
	constexpr double kRootRateDivisor = 576.0;

	constexpr std::uint16_t kFlagKeyOn = 0x4000;
	constexpr std::uint16_t kFlagKeyOff = 0x2000;
	constexpr std::uint16_t kFlagLoop = 0x0002;
	constexpr std::uint16_t kFlagMuLaw = 0x0008;
	constexpr std::uint16_t kFlagNoise = 0x0010;

	// the flags a sample's measurement depends on: mu-law bytes are decoded first, and the loop bit
	// decides which part of the sample is analysed
	constexpr std::uint16_t kMeasurementFlags = (std::uint16_t) (kFlagLoop | kFlagMuLaw | kFlagNoise);

	/** The longest loop that is taken to be one cycle of a wavetable when the correlation finds no
		period in it.

		Namco's own drivers on this chip play whole songs from a single sixteen-byte waveform: the
		Galaga rips in the repository upload exactly one cycle, point `wave_end` at its last byte and
		loop from `wave_start`, and get their notes entirely from the frequency register. Sixteen
		bytes is shorter than the analysis window, so the correlation has nothing to measure - but a
		driver that loops a sample this short is not looping a drum, and the loop *is* the cycle. Two
		hundred and fifty-six bytes is the widest that stays plausible: a longer loop that does not
		correlate is a one-shot with noise in it, and the root rate is the honest answer there. */
	constexpr std::size_t kSingleCycleBytes = 256;

	/** The mask the core folds a sample address with (`pow2_mask` in EmuHelper.h): the ROM is
		addressed as a power-of-two buffer, so an address past the end wraps instead of reading
		elsewhere. Measured addresses are masked the same way, or a sample that wraps would be
		measured on bytes the chip never plays. */
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

	/** c352.c's mu-law decode table (`device_start_c352`): an 8-bit mu-law byte is a sign and a
		logarithmic magnitude, so it is decoded before being correlated - the period would survive
		either way, but the measurement's peak is cleaner on the linear values. */
	void buildMuLawTable (std::int16_t* table)
	{
		std::int16_t level = 0;
		for (int i = 0; i < 128; ++i)
		{
			table[i] = (std::int16_t) (level << 5);
			if (i < 16)
			{
				level = (std::int16_t) (level + 1);
			}
			else if (i < 24)
			{
				level = (std::int16_t) (level + 2);
			}
			else if (i < 48)
			{
				level = (std::int16_t) (level + 4);
			}
			else if (i < 100)
			{
				level = (std::int16_t) (level + 8);
			}
			else
			{
				level = (std::int16_t) (level + 16);
			}
		}

		for (int i = 128; i < 256; ++i)
		{
			table[i] = (std::int16_t) ((~table[i - 128]) & ~0x1F);
		}
	}

	/** One note table per voice: the mapping depends on the voice's sample as well as on the clock,
		because a sample only sounds at its recorded pitch at the rate it was recorded at.
		`cycleLength` points at the voice's cached estimate and is 0 until one is measured. */
	class C352NoteTable final : public NoteTable
	{
	public:
		C352NoteTable (double clock, const double* sampleCycle)
			: bytesPerSecondPerStep (clock / kClockDivisor)
			, rootRate (clock / kRootRateDivisor)
			, cycleLength (sampleCycle) {}

		/** L1 only: the transposition the rate register asks for against the chip's reference rate.
			The sample's own root is `rootCentsFromCycle()` beside it (design doc section 14), applied
			by the user's setting rather than baked into the mapping. */
		int noteFor (std::uint32_t period, int, int* finePitch) const override
		{
			return quantisePlaybackRate ((double) period * bytesPerSecondPerStep, rootRate, finePitch);
		}

	private:
		double bytesPerSecondPerStep;
		double rootRate;
		const double* cycleLength;
	};

	/** What a measurement belongs to: the registers that name the bytes of one sample. Two voices
		playing the same sample share the estimate, and a drum's failed measurement is remembered as
		"no period" rather than being retried on every key-on. */
	struct SampleKey
	{
		std::uint16_t bank;
		std::uint16_t start;
		std::uint16_t end;
		std::uint16_t loop;
		std::uint16_t flags;		// only kMeasurementFlags

		bool operator< (const SampleKey& other) const
		{
			return std::tie (bank, start, end, loop, flags)
				   < std::tie (other.bank, other.start, other.end, other.loop, other.flags);
		}
	};
}

struct C352Decoder::Impl
{
	struct Voice
	{
		std::uint16_t volumeFront = 0;	// +0: high byte left, low byte right
		std::uint16_t volumeRear = 0;	// +1: the same for the rear pair
		std::uint16_t frequency = 0;	// +2
		std::uint16_t flags = 0;		// +3
		std::uint16_t bank = 0;			// +4
		std::uint16_t start = 0;		// +5
		std::uint16_t end = 0;			// +6
		std::uint16_t loop = 0;			// +7

		// latched at the execute write, where the core copies the start address into the pointer
		bool keyed = false;
		std::uint32_t keyOns = 0;		// monotonic: a retrigger at the same pitch is a new note
		std::uint32_t gateEnd = 0;		// when a non-looping sample reaches its end
		double cycleLength = 0.0;		// the latched sample's own period; 0 = use the root rate
	};

	Voice voices[numChannels];
	std::uint32_t now = 0;
	double clock = (double) kDefaultClock;
	double streamRate = 44100.0;
	std::vector<std::uint8_t> rom;			// the song's 0x92 blocks, one byte per address
	std::uint32_t romMask = 0;				// the power-of-two window the core addresses inside
	std::int16_t muLaw[256] = {};
	std::map<SampleKey, double> cycleCache;
	std::vector<std::unique_ptr<C352NoteTable>> tables;

	static bool loops (const Voice& voice)
	{
		// REVLOOP (0x0003) is the loop bit plus reverse: both never reach the end address
		return (voice.flags & kFlagLoop) != 0;
	}

	/** How long a non-looping voice lasts, in stream samples.

		The core stops the voice when the pointer reaches `wave_end`, so the length is the sample's
		own size divided by the rate the chip advances at. A frequency of zero never carries the
		16.16 counter over, so no byte is ever fetched and the voice is silence; an end at or
		below the start is not a sample the format's drivers write, and treating it as silence is
		better than inventing a length from a wrapped pointer. */
	std::uint32_t durationOf (const Voice& voice) const
	{
		if (loops (voice) || voice.end <= voice.start || voice.frequency == 0)
		{
			return 0;
		}

		// the core fetches the byte at `wave_end` and only then decides the sample is over
		// (c352.c, C352_fetch_sample), so both ends of the region are part of it
		const double bytesPerSecond = (double) voice.frequency * clock / kClockDivisor;
		const double samples = (double) (voice.end - voice.start + 1) / bytesPerSecond * streamRate;
		return (std::uint32_t) std::llround (std::min (samples, 1.0e9));
	}

	/** The cycle length of the sample the voice is about to play, measured once per sample.

		The registers name the sample: the bank and the start address pick where it begins, the loop
		register says which part repeats, and the mode says whether the bytes are linear PCM or
		mu-law (decoded first). A noise voice has no bytes at all, and a sample with no loop is
		measured over its whole size - the attack may not repeat, which is what the correlation's
		"nothing repeats" answer is for.

		A loop too short to hold one correlation window falls back to "the loop is one cycle"
		(`kSingleCycleBytes`): the wavetable songs on this chip are a single sixteen-byte cycle, and
		without that answer they would all be quantised against the root rate, three octaves off. */
	double cycleFor (const Voice& voice)
	{
		if (rom.empty() || (voice.flags & kFlagNoise) != 0 || voice.end <= voice.start)
		{
			return 0.0;
		}

		const SampleKey key { voice.bank, voice.start, voice.end, voice.loop,
							  (std::uint16_t) (voice.flags & kMeasurementFlags) };
		const auto cached = cycleCache.find (key);
		if (cached != cycleCache.end())
		{
			return cached->second;
		}

		// `wave_end` is played - the core fetches the byte and only then checks for the end - so the
		// region and the loop run to it inclusively. One byte matters: a sixteen-byte wavetable
		// cycle is a semitone away from a fifteen-byte one.
		const std::uint32_t address = ((std::uint32_t) voice.bank << 16) | voice.start;
		const std::size_t size = (std::size_t) (voice.end - voice.start) + 1;
		std::vector<double> waveform (size);
		for (std::size_t i = 0; i < size; ++i)
		{
			const std::uint32_t index = (address + (std::uint32_t) i) & romMask;
			const std::uint8_t byte = (index < rom.size()) ? rom[index] : 0;
			waveform[i] = (voice.flags & kFlagMuLaw) != 0 ? (double) muLaw[byte]
														 : (double) (std::int8_t) byte;
		}

		std::size_t loopOffset = 0;
		std::size_t loopLength = 0;
		if (loops (voice) && voice.loop >= voice.start && voice.loop <= voice.end)
		{
			loopOffset = (std::size_t) (voice.loop - voice.start);
			loopLength = size - loopOffset;
		}

		double cycle = estimateCycleLength (waveform, loopOffset, loopLength);
		if (cycle <= 0.0 && loopLength > 0 && loopLength <= kSingleCycleBytes)
		{
			cycle = (double) loopLength;		// a wavetable driver loops exactly one cycle
		}

		cycleCache[key] = cycle;
		return cycle;
	}

	/** The write to 0x202 that runs every voice's key-on / key-off request - the only place a voice
		changes state, which is why both edges are read from the flag register here and not from the
		write that set the flag. The request bit is cleared afterwards, the way the core clears it:
		a second execute write with no new flag write must do nothing. */
	void execute (std::uint32_t sampleTime)
	{
		for (auto& voice : voices)
		{
			if ((voice.flags & kFlagKeyOn) != 0)
			{
				voice.flags = (std::uint16_t) (voice.flags & ~kFlagKeyOn);
				voice.keyed = true;
				voice.gateEnd = sampleTime + durationOf (voice);
				voice.cycleLength = cycleFor (voice);
			}
			else if ((voice.flags & kFlagKeyOff) != 0)
			{
				voice.flags = (std::uint16_t) (voice.flags & ~kFlagKeyOff);
				voice.keyed = false;
				voice.gateEnd = 0;
			}
		}
	}

	int volumeOf (const Voice& voice) const
	{
		// four output channels: the front pair is louder in most mixes, but a voice panned to the
		// rear alone still sounds, so the loudest of the four is the level
		const int front = std::max ((int) (voice.volumeFront >> 8), (int) (voice.volumeFront & 0xFF));
		const int rear = std::max ((int) (voice.volumeRear >> 8), (int) (voice.volumeRear & 0xFF));
		return std::min (15, (std::max (front, rear) + 8) >> 4);
	}

	bool isSounding (int channel) const
	{
		const Voice& voice = voices[channel];
		if (! voice.keyed || voice.frequency == 0)
		{
			return false;
		}

		if (volumeOf (voice) == 0)
		{
			return false;		// keyed, but every output is attenuated to nothing
		}

		// the loop bit is read live by the core, so a looping voice only a key-off can stop
		return loops (voice) || now < voice.gateEnd;
	}
};

C352Decoder::C352Decoder() : impl (std::make_unique<Impl>())
{
	reset (kDefaultClock, 44100.0);
}

C352Decoder::~C352Decoder() = default;

void C352Decoder::reset (std::uint32_t clock, double sampleRate)
{
	// the ROM belongs to the song, not to the clock: a reset only changes the rate the chip runs
	// at, so it is carried over instead of being dropped with the voices
	std::vector<std::uint8_t> rom = impl != nullptr ? std::move (impl->rom) : std::vector<std::uint8_t>();
	const std::uint32_t romMask = impl != nullptr ? impl->romMask : 0u;

	impl = std::make_unique<Impl>();
	impl->clock = (clock > 100000) ? (double) clock : (double) kDefaultClock;
	impl->streamRate = (sampleRate > 0.0) ? sampleRate : 44100.0;
	impl->rom = std::move (rom);
	impl->romMask = romMask;
	buildMuLawTable (impl->muLaw);
	impl->tables.reserve ((std::size_t) numChannels);
	for (auto& voice : impl->voices)
	{
		impl->tables.push_back (std::make_unique<C352NoteTable> (impl->clock, &voice.cycleLength));
	}
}

void C352Decoder::setRom (std::vector<std::uint8_t> rom)
{
	impl->romMask = pow2Mask (rom.size());
	impl->rom = std::move (rom);
	impl->cycleCache.clear();	// the samples describe a different ROM now
}

void C352Decoder::write (const RegisterWrite& w)
{
	// the VGM command carries a 16-bit offset and a 16-bit value, both big-endian; the reader puts
	// the offset's high byte in `port` and the value's low byte in `value2`
	const std::uint16_t offset = (std::uint16_t) (((std::uint16_t) (w.port & 0x7F) << 8) | w.reg);
	const std::uint16_t value = (std::uint16_t) (((std::uint16_t) w.value << 8) | w.value2);

	if (offset >= 0x100)
	{
		// 0x200 is the control register, whose purpose is unknown and which no note depends on;
		// 0x202 is the execute write every key-on and key-off travels through
		if (offset == 0x202)
		{
			impl->execute (w.sampleTime);
		}
		return;
	}

	const int channel = (int) (offset >> 3);
	Impl::Voice& voice = impl->voices[channel];

	switch (offset & 7)
	{
	case 0:
		voice.volumeFront = value;
		break;
	case 1:
		voice.volumeRear = value;
		break;
	case 2:
		voice.frequency = value;
		break;
	case 3:
		voice.flags = value;
		break;
	case 4:
		voice.bank = value;
		break;
	case 5:
		voice.start = value;
		break;
	case 6:
		voice.end = value;
		break;
	case 7:
		voice.loop = value;
		break;
	default:
		break;
	}
}

void C352Decoder::advance (std::uint32_t sampleTime)
{
	impl->now = sampleTime;

	for (auto& voice : impl->voices)
	{
		// the core clears the busy flag when a non-looping sample reaches its end; with the loop
		// bit it never does, and the voice stays keyed until a key-off says otherwise
		if (voice.keyed && ! Impl::loops (voice) && sampleTime >= voice.gateEnd)
		{
			voice.keyed = false;
		}
	}
}

ChannelState C352Decoder::channelState (int channel) const
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
	state.period = voice.frequency;		// the note table turns the rate into a note
	state.volume = impl->volumeOf (voice);

	// the C352 has no drum convention of its own: every voice is a sample, and one whose period
	// cannot be measured simply has no root to add
	const SampledPitch pitch =
		describeSamplePitch ((double) voice.frequency * impl->clock / kClockDivisor,
							 impl->clock / kRootRateDivisor, voice.cycleLength, true);
	state.hz = pitch.hz;
	state.transposeCents = pitch.transposeCents;
	state.rootCents = pitch.rootCents;
	state.pitchSource = pitch.source;

	return state;
}

const NoteTable& C352Decoder::noteTable (int channel) const
{
	// one table per voice: each voice's note is its rate divided by the cycle length of its own
	// sample, and two voices can be playing samples recorded at completely different rates
	const std::size_t index = (channel >= 0 && channel < channelCount_) ? (std::size_t) channel : 0;
	return *impl->tables[index];
}

Family C352Decoder::family (int channel) const
{
	(void) channel;
	return Family::c352Pcm;
}

} // namespace toybox::chips
