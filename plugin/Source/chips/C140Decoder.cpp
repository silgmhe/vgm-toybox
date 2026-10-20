#include "C140Decoder.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	// System 2 boards run the C140 at 8 MHz; the header clock is used when it is there.
	constexpr std::uint32_t kDefaultClock = 8000000;

	// c140.c: the sample pointer advances by `frequency * clock / 288` bytes a second, and the
	// VGM's 16-bit frequency is multiplied by that in 16.16 (`pbase = clock / 288 / sampleRate`).
	// Dividing by 288 * 65536 turns the register and the clock into bytes a second.
	constexpr double kClockDivisor = 18874368.0;

	// The rate at which a C140 sample ROM is taken to play at its recorded pitch. The chip does not
	// say, and the games' drivers disagree: 8-bit PCM at 8 kHz is the common case for the boards.
	// It is only the fallback: a sample whose own cycle length can be measured is played at the
	// pitch it was recorded at, and this constant covers the rest (drums, noise, mu-law, no ROM).
	constexpr double kRootRate = 8000.0;

	constexpr std::uint8_t kModeMulaw = 0x08;
	constexpr std::uint8_t kModeLoop = 0x10;
	constexpr std::uint8_t kModeKeyOn = 0x80;

	/** The length of one cycle of a recorded sample, in bytes.

		The chip never states it: a C140 sample is raw 8-bit PCM, and a note is the playback rate
		divided by this length. The measurement itself is `chips::estimateCycleLength()` in
		Pitch.h, shared with the other sampled players (the C352 is the second one) - it takes the
		waveform as doubles so a sampled chip can hand over decoded mu-law bytes. This wrapper only
		turns the sample's bytes into that waveform; the loop argument says which part repeats.

		Returns 0 for a sample whose period does not stand out (a drum, noise, a one-shot with no
		loop), which the caller reads as "use the root rate". */
	double estimateSamplePeriod (const std::uint8_t* sample, std::size_t size,
								 std::size_t loopOffset, std::size_t loopLength)
	{
		if (sample == nullptr)
		{
			return 0.0;
		}

		const std::vector<double> waveform (sample, sample + size);
		return estimateCycleLength (waveform, loopOffset, loopLength);
	}

	/** One note table per voice: the mapping now depends on the voice's sample as well as on the
		clock, because a sample only sounds at its recorded pitch at the rate it was recorded at.
		`samplePeriodBytes` points at the voice's cached estimate and is 0 until one is measured. */
	class C140NoteTable final : public NoteTable
	{
	public:
		C140NoteTable (double clock, const double* samplePeriod)
			: bytesPerSecondPerStep (clock / kClockDivisor), samplePeriodBytes (samplePeriod) {}

		int noteFor (std::uint32_t period, int, int* finePitch) const override
		{
			const double rate = (double) period * bytesPerSecondPerStep;
			if (samplePeriodBytes != nullptr && *samplePeriodBytes > 0.0)
			{
				return quantiseFrequency (rate / *samplePeriodBytes, finePitch);
			}

			return quantisePlaybackRate (rate, kRootRate, finePitch);
		}

	private:
		double bytesPerSecondPerStep;
		const double* samplePeriodBytes;
	};
}

struct C140Decoder::Impl
{
	struct Channel
	{
		std::uint8_t volumeRight = 0;	// +0
		std::uint8_t volumeLeft = 0;	// +1
		std::uint8_t freqHi = 0;		// +2
		std::uint8_t freqLo = 0;		// +3
		std::uint8_t bank = 0;			// +4
		std::uint8_t mode = 0;			// +5: 0x08 mu-law, 0x10 loop, 0x80 key on
		std::uint8_t startHi = 0;		// +6
		std::uint8_t startLo = 0;		// +7
		std::uint8_t endHi = 0;			// +8
		std::uint8_t endLo = 0;			// +9
		std::uint8_t loopHi = 0;		// +A
		std::uint8_t loopLo = 0;		// +B

		// latched at key-on, where the core copies start/end into the voice
		bool keyed = false;
		std::uint32_t gateEnd = 0;		// when a non-looping sample reaches its end
		double samplePeriodBytes = 0.0;	// the latched sample's cycle length; 0 = use the root rate
	};

	Channel channels[numChannels];
	std::uint32_t now = 0;
	double clock = (double) kDefaultClock;
	double streamRate = 44100.0;
	std::vector<std::uint8_t> rom;					// the song's 0x8D blocks, one byte per address
	Banking banking = Banking::system2;				// how a voice's bank/address reaches that ROM
	std::map<std::uint64_t, double> periodCache;	// sample description -> cycle length
	std::vector<std::unique_ptr<C140NoteTable>> tables;

	std::uint32_t startOf (const Channel& channel) const
	{
		return ((std::uint32_t) channel.startHi << 8) | channel.startLo;
	}

	std::uint32_t endOf (const Channel& channel) const
	{
		return ((std::uint32_t) channel.endHi << 8) | channel.endLo;
	}

	std::uint32_t loopOf (const Channel& channel) const
	{
		return ((std::uint32_t) channel.loopHi << 8) | channel.loopLo;
	}

	/** The ROM offset a voice's bank and address point at - libvgm's `find_sample()` (c140.c),
		which is where the board's wiring lives. The registers only carry eight bits of bank, so
		on System 2 the address is folded into 19 bits: without that fold a one-megabyte ROM has
		no bank 0x14 and every sample of the song comes out unmeasurable. */
	std::uint32_t sampleOffset (std::uint32_t address, std::uint8_t bank) const
	{
		const std::uint32_t adrs = ((std::uint32_t) bank << 16) + address;
		switch (banking)
		{
		case Banking::system21:
			return ((adrs & 0x300000u) >> 1) + (adrs & 0x7FFFFu);

		case Banking::linear:
			return adrs;

		case Banking::system2:
		default:
			return ((adrs & 0x200000u) >> 2) | (adrs & 0x7FFFFu);
		}
	}

	/** The cycle length of the sample the voice is about to play, measured once per sample.

		The registers name the sample: the bank and the start address pick where it begins, the
		loop says which part repeats, and the mode says whether the bytes are linear PCM at all.
		mu-law samples are not, so a correlation over their bytes means nothing - they keep the
		root rate. A sample that begins outside the ROM, or one whose loop is shorter than the
		correlation window needs, does the same. */
	double samplePeriodFor (const Channel& channel)
	{
		const std::uint32_t start = startOf (channel);
		const std::uint32_t end = endOf (channel);
		const std::uint32_t loop = loopOf (channel);
		if (rom.empty() || end <= start || (channel.mode & kModeMulaw) != 0)
		{
			return 0.0;
		}

		const std::uint64_t key = ((std::uint64_t) channel.bank << 48) | ((std::uint64_t) start << 32)
								  | ((std::uint64_t) end << 16) | (std::uint64_t) loop;
		const auto cached = periodCache.find (key);
		if (cached != periodCache.end())
		{
			return cached->second;
		}

		const std::uint32_t address = sampleOffset (start, channel.bank);
		double period = 0.0;
		if (address < (std::uint64_t) rom.size())
		{
			const std::size_t size = std::min<std::size_t> ((std::size_t) (end - start),
															rom.size() - (std::size_t) address);
			const bool loops = (channel.mode & kModeLoop) != 0 && loop >= start && loop < end;
			const std::size_t loopOffset = loops ? (std::size_t) (loop - start) : 0;
			const std::size_t loopLength = loops ? size - loopOffset : 0;
			period = estimateSamplePeriod (rom.data() + address, size, loopOffset, loopLength);
		}

		periodCache[key] = period;
		return period;
	}

	static std::uint32_t frequencyOf (const Channel& channel)
	{
		return ((std::uint32_t) channel.freqHi << 8) | channel.freqLo;
	}

	/** How long a non-looping voice lasts, in stream samples.

		The core stops the voice when the pointer reaches `end - start` bytes, so the length is the
		sample's own size divided by the rate the chip advances at. A voice with no frequency is
		skipped by the core entirely (`if (frequency == 0) continue;`) and an end at or below the
		start trips the stop on the first sample: both are silence, not a note. */
	std::uint32_t durationOf (const Channel& channel) const
	{
		const std::uint32_t start = startOf (channel);
		const std::uint32_t end = endOf (channel);
		const std::uint32_t frequency = frequencyOf (channel);
		if (end <= start || frequency == 0)
		{
			return 0;
		}

		const double bytesPerSecond = (double) frequency * clock / kClockDivisor;
		const double samples = (double) (end - start) / bytesPerSecond * streamRate;
		return (std::uint32_t) std::llround (std::min (samples, 1.0e9));
	}

	void keyWrite (int channel, std::uint8_t value, std::uint32_t sampleTime)
	{
		Channel& voice = channels[channel];
		voice.mode = value;

		// any mode write without the key bit stops the voice - c140.c clears `key` there, and a
		// driver uses exactly that as its key-off
		if ((value & kModeKeyOn) == 0)
		{
			voice.keyed = false;
			voice.gateEnd = 0;
			return;
		}

		// the key-on edge is where the core latches start/end/loop, so the length and the sample's
		// own pitch are fixed here as well
		voice.keyed = true;
		voice.gateEnd = sampleTime + durationOf (voice);
		voice.samplePeriodBytes = samplePeriodFor (voice);
	}

	bool isSounding (int channel) const
	{
		const Channel& voice = channels[channel];
		if (! voice.keyed || frequencyOf (voice) == 0)
		{
			return false;
		}

		if (voice.volumeLeft == 0 && voice.volumeRight == 0)
		{
			return false;	// keyed, but both outputs are attenuated to nothing
		}

		// the loop bit is read live by the core, so a looping voice only a key-off can stop
		return (voice.mode & kModeLoop) != 0 || now < voice.gateEnd;
	}

	int volumeOf (const Channel& channel) const
	{
		const int level = std::max ((int) channel.volumeLeft, (int) channel.volumeRight);
		return std::min (15, (level + 8) >> 4);
	}
};

C140Decoder::C140Decoder() : impl (std::make_unique<Impl>())
{
	reset (kDefaultClock, 44100.0);
}

C140Decoder::~C140Decoder() = default;

void C140Decoder::reset (std::uint32_t clock, double sampleRate)
{
	// the ROM and its banking belong to the song, not to the clock: a reset only changes the rate
	// the chip runs at, so both are carried over instead of being dropped with the voices
	std::vector<std::uint8_t> rom = impl != nullptr ? std::move (impl->rom) : std::vector<std::uint8_t>();
	const Banking banking = impl != nullptr ? impl->banking : Banking::system2;

	impl = std::make_unique<Impl>();
	impl->clock = (clock > 100000) ? (double) clock : (double) kDefaultClock;
	impl->streamRate = (sampleRate > 0.0) ? sampleRate : 44100.0;
	impl->rom = std::move (rom);
	impl->banking = banking;
	impl->tables.reserve ((std::size_t) numChannels);
	for (auto& voice : impl->channels)
	{
		impl->tables.push_back (std::make_unique<C140NoteTable> (impl->clock, &voice.samplePeriodBytes));
	}
}

C140Decoder::Banking C140Decoder::bankingFromHeaderByte (std::uint8_t headerByte)
{
	switch (headerByte)
	{
	case (std::uint8_t) Banking::system21:
		return Banking::system21;

	case (std::uint8_t) Banking::linear:
		return Banking::linear;

	default:
		// 0x00 is System 2, and so is anything else: 0x02 is the ASIC 219, which libvgm plays
		// through a different device, and a file too old to carry the field has no byte 0x96 at
		// all. Guessing a fold that the board does not apply is worse than the default the
		// player itself uses for an unknown value.
		return Banking::system2;
	}
}

void C140Decoder::setRom (std::vector<std::uint8_t> rom, Banking newBanking)
{
	impl->rom = std::move (rom);
	impl->banking = newBanking;
	impl->periodCache.clear();	// the samples describe a different ROM now
}

void C140Decoder::write (const RegisterWrite& w)
{
	// the VGM command carries a 16-bit offset; the voices are the low 0x180 of it and the interval
	// timer above that says nothing about which note is playing
	const int reg = ((int) w.port << 8) | (int) w.reg;
	if (reg < 0 || reg >= (int) numChannels * 16)
	{
		return;
	}

	const int channel = reg >> 4;
	Impl::Channel& voice = impl->channels[channel];

	switch (reg & 0x0F)
	{
	case 0x0:
		voice.volumeRight = w.value;
		break;
	case 0x1:
		voice.volumeLeft = w.value;
		break;
	case 0x2:
		voice.freqHi = w.value;
		break;
	case 0x3:
		voice.freqLo = w.value;
		break;
	case 0x4:
		voice.bank = w.value;
		break;
	case 0x5:
		impl->keyWrite (channel, w.value, w.sampleTime);
		break;
	case 0x6:
		voice.startHi = w.value;
		break;
	case 0x7:
		voice.startLo = w.value;
		break;
	case 0x8:
		voice.endHi = w.value;
		break;
	case 0x9:
		voice.endLo = w.value;
		break;
	case 0xA:
		voice.loopHi = w.value;
		break;
	case 0xB:
		voice.loopLo = w.value;
		break;
	default:
		break;		// 0xC..0xF: reserved
	}
}

void C140Decoder::advance (std::uint32_t sampleTime)
{
	impl->now = sampleTime;

	for (auto& voice : impl->channels)
	{
		// the core clears the key when a non-looping sample reaches its end; with the loop bit it
		// never does, and the voice stays keyed until a mode write says otherwise
		if (voice.keyed && (voice.mode & kModeLoop) == 0 && sampleTime >= voice.gateEnd)
		{
			voice.keyed = false;
		}
	}
}

ChannelState C140Decoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= channelCount_)
	{
		return state;
	}

	const Impl::Channel& voice = impl->channels[channel];
	state.present = true;
	state.gate = impl->isSounding (channel);
	state.period = Impl::frequencyOf (voice);		// the note table turns the rate into a note
	state.volume = impl->volumeOf (voice);

	return state;
}

const NoteTable& C140Decoder::noteTable (int channel) const
{
	// one table per voice: each voice's note is its rate divided by the cycle length of its own
	// sample, and two voices can be playing samples recorded at completely different rates
	const std::size_t index = (channel >= 0 && channel < channelCount_) ? (std::size_t) channel : 0;
	return *impl->tables[index];
}

Family C140Decoder::family (int channel) const
{
	(void) channel;
	return Family::c140Pcm;
}

} // namespace toybox::chips
