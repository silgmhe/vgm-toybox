#include "C140Decoder.h"

#include <algorithm>
#include <cmath>

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
	// say, and the games' drivers disagree: 8-bit PCM at 8 kHz is the common case for the boards,
	// and it is one constant so the root-note override of section 11.1 can replace it in one place.
	constexpr double kRootRate = 8000.0;

	constexpr std::uint8_t kModeMulaw = 0x08;
	constexpr std::uint8_t kModeLoop = 0x10;
	constexpr std::uint8_t kModeKeyOn = 0x80;

	class C140NoteTable final : public NoteTable
	{
	public:
		explicit C140NoteTable (double clock) : bytesPerSecondPerStep (clock / kClockDivisor) {}

		int noteFor (std::uint32_t period, int, int* finePitch) const override
		{
			return quantisePlaybackRate ((double) period * bytesPerSecondPerStep, kRootRate, finePitch);
		}

	private:
		double bytesPerSecondPerStep;
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
	};

	Channel channels[numChannels];
	std::uint32_t now = 0;
	double clock = (double) kDefaultClock;
	double streamRate = 44100.0;
	std::unique_ptr<C140NoteTable> table;

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
		const std::uint32_t start = ((std::uint32_t) channel.startHi << 8) | channel.startLo;
		const std::uint32_t end = ((std::uint32_t) channel.endHi << 8) | channel.endLo;
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

		// the key-on edge is where the core latches start/end/loop, so the length is fixed here
		voice.keyed = true;
		voice.gateEnd = sampleTime + durationOf (voice);
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
	impl = std::make_unique<Impl>();
	impl->clock = (clock > 100000) ? (double) clock : (double) kDefaultClock;
	impl->streamRate = (sampleRate > 0.0) ? sampleRate : 44100.0;
	impl->table = std::make_unique<C140NoteTable> (impl->clock);
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
	(void) channel;		// one mapping for every voice: a playback rate and the root rate
	return *impl->table;
}

Family C140Decoder::family (int channel) const
{
	(void) channel;
	return Family::c140Pcm;
}

} // namespace toybox::chips
