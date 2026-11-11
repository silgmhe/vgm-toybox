#include "HuC6280Decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	constexpr uint32_t kDefaultClock = 3579545;

	// A channel with noise switched on ignores its frequency register; its rate comes from the
	// noise control register instead. Reported in this bit so one table can serve both cases -
	// 12 bits cannot hold the frequency and a marker at the same time.
	constexpr uint32_t kNoiseFlag = 0x1000;

	/** The channel-select register's low three bits name the channel that the next four writes
		address, and the low four bits of an offset are the register inside that block - the chip
		has one register file shared by six channels rather than six files. */
	constexpr uint8_t kChannelSelectMask = 0x07;
	constexpr uint8_t kRegisterIndexMask = 0x0F;

	/** The noise channel's rate is five bits, and its period index is what the drum table reads. */
	constexpr uint32_t kNoiseRateMask = 0x1F;

	/** One table: thirty-two steps of five bits (`c6280_mame.c:96`). */
	constexpr int kWaveSteps = 32;

	/** The table's midpoint. Five bits are unsigned on the wire and centred on 16 - the chip's
		silence is 16, not 0 - so a decoded step is `(value - 16) / 16`, in -1..+0.9375. */
	constexpr float kWaveCentre = 16.0f;

	/** The index wraps at the table length; `0x06` writes past it would be a driver bug. */
	constexpr int kWaveIndexMask = kWaveSteps - 1;

	/** A driver loads one to three tables per channel, but this is a cache with a bound rather than
		a container that grows: a song that rewrites a table every frame must not leak memory. */
	constexpr std::size_t kWaveCacheLimit = 64;

	/** FNV-1a over the table's bytes: two channels that load the same shape share one decode. */
	uint64_t hashOfTable (const uint8_t* bytes)
	{
		uint64_t hash = 1469598103934665603ull;

		for (int i = 0; i < kWaveSteps; ++i)
		{
			hash = (hash ^ (uint64_t) bytes[i]) * 1099511628211ull;
		}

		return hash;
	}

	/** Wave: the core steps the wavetable once every 4096 counter ticks with a step of
		(clock / rate) * 4096 / f, so one step lasts f / clock seconds and the 32 steps of the
		wavetable one cycle: frequency = clock / (32 * f). Register 0 lands in the table's slot for
		4096, which makes it the lowest pitch rather than the highest.

		Noise: the LFSR is clocked 32 times per noise count and shifts every 2048 ticks, giving
		clock / (64 * (index + 1)) with index 0 the fastest. There is no pitch, so the rate picks
		a GM drum key; the 32 rates span exactly five octaves, which fits the eleven keys the 2A03
		noise uses as well. */
	class HuC6280Table final : public NoteTable
	{
	public:
		explicit HuC6280Table (double clock) : clock (clock) {}

		int noteFor (uint32_t period, int, int* finePitch) const override
		{
			if ((period & kNoiseFlag) != 0)
			{
				return noiseNote (period & kNoiseRateMask, finePitch);
			}

			const double divider = (period == 0) ? 4096.0 : (double) period;
			return quantiseFrequency (clock / (32.0 * divider), finePitch);
		}

	private:
		int noiseNote (uint32_t index, int* finePitch) const
		{
			if (finePitch != nullptr)
			{
				*finePitch = 0;
			}

			// 0 is the fastest rate, 31 the slowest; position 0..1 across those five octaves
			const double hz = clock / (64.0 * (double) (index + 1));
			const double position = std::log2 ((clock / 64.0) / std::max (hz, 0.001)) / 5.0;
			const long level = std::lround (position * 10.0);

			return kDrumBaseKey + (int) std::clamp<long> (level, 0, 10);
		}

		static constexpr int kDrumBaseKey = 38;		// GM acoustic snare, where the 2A03 starts

		double clock;
	};
}

struct HuC6280Decoder::Impl
{
	struct Channel
	{
		int frequency = 0;				// 12 bits, registers 0x02/0x03
		uint8_t control = 0;		// 0x04: bit 7 enable, bit 6 DDA, bits 4-0 volume
		uint8_t noiseControl = 0;	// 0x07: bit 7 noise on, bits 4-0 rate

		/** **The thirty-two steps this channel walks**, five bits each, and where the next write goes.

			The core writes at `waveIndex` and only advances it while the channel is keyed **off**
			(`c6280_mame.c:240`): a driver loads a table while the channel is silent, key-on resets
			the counter but not the index, and turning DDA off puts the index back to zero
			(`c6280_mame.c:219-222`). Following that exactly is what keeps the row's shape aligned
			with the steps the chip actually plays - an index that advanced while keyed on would put
			every later write one step off.
		*/
		uint8_t waveform[kWaveSteps] = {};
		uint8_t waveIndex = 0;
	};

	// the select register is three bits wide, and a driver may address 6/7; keep room for them
	Channel channels[8];
	uint8_t select = 0;
	double clock = (double) kDefaultClock;
	std::unique_ptr<HuC6280Table> table;

	/** Decoded tables, keyed by their thirty-two bytes.

		`channelState` is asked once per row per frame - six times - while the driver may be rewriting
		a table every frame, so the decode has to be a cache hit in the common case. `mutable`
		because `channelState` is const and the table it hands out is immutable once decoded.
	*/
	mutable std::map<uint64_t, std::shared_ptr<const std::vector<float>>> waveCache;

	Channel& selected (void) { return channels[select & kChannelSelectMask]; }

	/** The decoded table of one channel, reusing a previous decode when the bytes are the same. */
	std::shared_ptr<const std::vector<float>> waveFor (int channel) const
	{
		const Channel& chan = channels[channel];
		const uint64_t key = hashOfTable (chan.waveform);

		const auto found = waveCache.find (key);

		if (found != waveCache.end ())
		{
			return found->second;
		}

		auto wave = std::make_shared<std::vector<float>> ();
		wave->reserve (kWaveSteps);

		for (int i = 0; i < kWaveSteps; ++i)
		{
			wave->push_back (((float) chan.waveform[i] - kWaveCentre) / kWaveCentre);
		}

		if (waveCache.size () >= kWaveCacheLimit)
		{
			// One song cannot need more than a handful of distinct shapes; a driver that writes a
			// new table every frame would otherwise grow this for the length of the song.
			waveCache.clear ();
		}

		return waveCache.emplace (key, std::move (wave)).first->second;
	}
};

HuC6280Decoder::HuC6280Decoder() : impl (std::make_unique<Impl>())
{
	reset (ChipConfig { kDefaultClock, 44100.0 });
}

HuC6280Decoder::~HuC6280Decoder() = default;

void HuC6280Decoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	(void) sampleRate;		// the chip has no time base of its own; nothing here needs the rate

	impl = std::make_unique<Impl>();

	// a missing clock in the header is common: the PC Engine's PSG clock is the CPU clock
	impl->clock = (clock > 100000) ? (double) clock : (double) kDefaultClock;
	impl->table = std::make_unique<HuC6280Table> (impl->clock);
}

void HuC6280Decoder::write (const RegisterWrite& w)
{
	Impl::Channel& channel = impl->selected();

	switch (w.reg & kRegisterIndexMask)
	{
	// The HuC6280's six registers, by offset:
	//
	//   0x00  channel select: bits 2-0 say which channel the next four writes address
	//   0x01  global balance - panning only, which a MIDI note cannot carry
	//   0x02/0x03  channel frequency, low byte then the four high bits
	//   0x04  channel control: bit 7 enable, bit 6 DDA mode, bits 4-0 volume
	//   0x05  channel balance - panning only, like 0x01
	//   0x06  waveform data - the shape, not the note
	//   0x07  noise control: bit 7 enable, bits 4-0 the noise frequency
	case 0x00:
		impl->select = (uint8_t) (w.value & 0x07);
		break;
	case 0x01:
		break;		// global balance: panning only, a MIDI note cannot carry it
	case 0x02:
		channel.frequency = (channel.frequency & 0x0F00) | w.value;
		break;
	case 0x03:
		channel.frequency = (channel.frequency & 0x00FF) | ((w.value & 0x0F) << 8);
		break;
	case 0x04:
		// **The DDA bit owns the table's read pointer.** Turning DDA off puts the index back to
		// zero (`c6280_mame.c:219-222`), which is why a driver can load a fresh table for the next
		// note without exposing a pointer register of its own.
		if ((channel.control & 0x40) != 0 && (w.value & 0x40) == 0)
		{
			channel.waveIndex = 0;
		}

		channel.control = (uint8_t) w.value;
		break;
	case 0x05:
		break;		// channel balance: panning only
	case 0x06:
		// **One register, two meanings.** With DDA on, `0x06` is a sample level rather than part of
		// a table (`c6280_mame.c:236-251`); this decoder models the wavetable, so those bytes are
		// left out instead of corrupting the shape the row draws.
		if ((channel.control & 0x40) == 0)
		{
			channel.waveform[channel.waveIndex] = (uint8_t) (w.value & 0x1F);

			// A keyed channel keeps pointing at the step it is playing; only a silent one advances,
			// which is exactly how the core loads a table while the channel is off.
			if ((channel.control & 0x80) == 0)
			{
				channel.waveIndex = (uint8_t) ((channel.waveIndex + 1) & kWaveIndexMask);
			}
		}
		break;
	case 0x07:
		channel.noiseControl = w.value;
		break;
	default:
		break;		// 0x08/0x09 LFO: not modelled
	}
}

void HuC6280Decoder::advance (uint32_t sampleTime)
{
	(void) sampleTime;		// no timers: the driver keys the channel on and off
}

ChannelState HuC6280Decoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= channelCount_)
	{
		return state;
	}

	const Impl::Channel& current = impl->channels[channel];
	const int volume = current.control & 0x1F;
	const bool enabled = (current.control & 0x80) != 0;
	const bool dda = (current.control & 0x40) != 0;
	const bool noise = channel >= channel4 && (current.noiseControl & 0x80) != 0;

	state.present = true;
	state.gate = enabled && ! dda && volume > 0;
	state.volume = volume * 15 / 31;		// the chip's 0..31 master volume onto the 0..15 scale
	state.patch = 0;

	if (noise)
	{
		// the core uses (noise_control & 0x1F) ^ 0x1F: the register counts down in rate
		state.period = kNoiseFlag | ((current.noiseControl & 0x1F) ^ 0x1F);
	}
	else
	{
		state.period = (uint32_t) current.frequency;

		// **The row draws the table the channel walks**, one cycle by definition, at the rate the
		// counter steps it: `clock / f` steps a second - the tone is that rate over thirty-two
		// steps, which is why the note table divides by `32 * f`. `f == 0` clocks at 4096, the same
		// substitution the note table makes, so the number here and the note agree.
		state.wave = impl->waveFor (channel);
		state.tableWave = true;
		state.sampleRate = (float) (impl->clock / (double) (current.frequency != 0 ? current.frequency
																				  : 4096));
	}

	return state;
}

const NoteTable& HuC6280Decoder::noteTable (int channel) const
{
	(void) channel;		// one table: the wavetable and the noise rate differ only in the period
	return *impl->table;
}

ChannelKind HuC6280Decoder::channelKind (int channel) const
{
	// Only the last two channels reach the LFSR, and only while the bit is on; the row then draws
	// noise rather than a table it is no longer walking (`c6280_mame.c:246-250`).
	if (channel >= channel4 && channel < channelCount_
		&& (impl->channels[channel].noiseControl & 0x80) != 0)
	{
		return ChannelKind::c6280Noise;
	}

	return ChannelKind::c6280Wave;
}

bool HuC6280Decoder::isDrum (int channel) const
{
	if (channel < channel4 || channel >= channelCount_)
	{
		return false;
	}

	return (impl->channels[channel].noiseControl & 0x80) != 0;
}

} // namespace toybox::chips
