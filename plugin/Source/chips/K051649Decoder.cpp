#include "K051649Decoder.h"

#include <algorithm>
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

	// the core stops a channel at this frequency: nine clocks cannot fill 32 waveform samples
	constexpr int kMinimumFrequency = 8;

	/** One table: thirty-two **signed** 8-bit samples (`k051649.c:141`). */
	constexpr int kWaveSteps = 32;
	constexpr int kWaveIndexMask = kWaveSteps - 1;

	/** SCC mode shares one table between channels 3 and 4; the split is the top bit of the address. */
	constexpr int kSharedTableStart = 0x60;

	/** A cache with a bound rather than a container that grows: a driver may rewrite a table every
		frame, and `channelState` is asked once per row per frame. */
	constexpr std::size_t kWaveCacheLimit = 64;

	/** FNV-1a over a table's bytes: two channels that hold the same shape share one decode - which is
		exactly what SCC mode needs, where channels 3 and 4 walk the same bytes. */
	uint64_t hashOfTable (const int8_t* bytes)
	{
		uint64_t hash = 1469598103934665603ull;

		for (int i = 0; i < kWaveSteps; ++i)
		{
			hash = (hash ^ (uint64_t) (uint8_t) bytes[i]) * 1099511628211ull;
		}

		return hash;
	}

	/** One wave cycle is 32 samples, each one clocked after 16 master clocks, and the register is
		the number of clocks between two samples minus one: clock / (16 * (register + 1)). */
	class SccTable final : public NoteTable
	{
	public:
		explicit SccTable (double clock) : clock (clock) {}

		int noteFor (uint32_t period, int, int* finePitch) const override
		{
			if (period > 0x0FFF)
			{
				return -1;
			}

			return quantiseFrequency (clock / (16.0 * ((double) period + 1.0)), finePitch);
		}

	private:
		double clock;
	};
}

struct K051649Decoder::Impl
{
	int frequency[numChannels] = {};
	uint8_t volume[numChannels] = {};
	bool key[numChannels] = {};

	/** **The thirty-two signed samples each channel walks.**

		The same RAM is addressed differently by the two chips: SCC shares one table between channels 3
		and 4 (any address at or above 0x60 writes both), while SCC+ (`modePlus`) gives channel 4 its
		own half at 0x80-0x9F. `k051649.c:268-271` and `:294-299` are the two writers, and the layout
		below follows them - a driver that writes 0x80-0x9F expects channel 4's table in SCC+ mode and
		gets channels 3 and 4 in SCC mode, exactly as the chip does.
	*/
	int8_t waveram[numChannels][kWaveSteps] = {};
	bool modePlus = false;

	uint8_t latch = 0;
	uint8_t test = 0;
	double clock = (double) kDefaultClock;
	std::unique_ptr<SccTable> table;

	/** Decoded tables keyed by their bytes. `mutable` because `channelState` is const and the table it
		hands out is immutable once decoded; the cache is bounded by `kWaveCacheLimit`. */
	mutable std::map<uint64_t, std::shared_ptr<const std::vector<float>>> waveCache;

	/** The decoded table of one channel, reusing a previous decode when the bytes are the same. */
	std::shared_ptr<const std::vector<float>> waveFor (int channel) const
	{
		const uint64_t key = hashOfTable (waveram[channel]);
		const auto found = waveCache.find (key);

		if (found != waveCache.end ())
		{
			return found->second;
		}

		auto wave = std::make_shared<std::vector<float>> ();
		wave->reserve (kWaveSteps);

		for (int i = 0; i < kWaveSteps; ++i)
		{
			// Signed on the wire: -128..127 maps to -1..+0.992, silence is 0.
			wave->push_back ((float) waveram[channel][i] / 128.0f);
		}

		if (waveCache.size () >= kWaveCacheLimit)
		{
			// A song needs a handful of distinct shapes; a driver that writes a new one every frame
			// must not grow this for the length of the song.
			waveCache.clear ();
		}

		return waveCache.emplace (key, std::move (wave)).first->second;
	}
};

K051649Decoder::K051649Decoder() : impl (std::make_unique<Impl>())
{
	reset (ChipConfig { kDefaultClock, 44100.0 });
}

K051649Decoder::~K051649Decoder() = default;

void K051649Decoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	(void) sampleRate;		// no timers: the key mask is the gate

	impl = std::make_unique<Impl>();

	impl->clock = (clock > 100000) ? (double) clock : (double) kDefaultClock;
	impl->table = std::make_unique<SccTable> (impl->clock);

	// **SCC or SCC+.** The VGM clock field's bit 31 says which chip the file expects
	// (`vgmplayer.cpp:1270`), and the replayer passes it here the way it passes an OPN variant:
	// without this the four SCC+ files draw channel 4 with channel 3's table.
	impl->modePlus = (config.flags & 1u) != 0;
}

void K051649Decoder::write (const RegisterWrite& w)
{
	// even offsets set the register index, the odd one after it carries the value
	if ((w.reg & 1) == 0)
	{
		impl->latch = w.value;
		return;
	}

	const uint8_t value = w.value;

	// The SCC's register halves, after the latch (the address is `reg >> 1`, which is why the
	// labels below are 0x00..0x05 rather than byte offsets):
	//
	//   0x00  waveform RAM          0x04  the same RAM, addressed rotated in SCC+ mode
	//   0x01  frequency: the latched address selects a channel and which half of the period
	//         (bit 0 = high bits), so one register number carries two different writes
	//   0x02  volume and channel enable, same latched selection
	//   0x03  key mask: the data byte says which channels run, the address only latches
	//   0x05  test register (waveform deformation), not a note
	switch (w.reg >> 1)
	{
	case 0x00:		// waveform data
	case 0x04:		// the same RAM, addressed rotated in SCC+ mode
	{
		const uint8_t offset = impl->latch;
		const uint8_t step = (uint8_t) (offset & kWaveIndexMask);

		// **The test register can make the RAM read-only** (`k051649.c:262-266`, `:297-299`): in SCC
		// mode both bits matter, in SCC+ only bit 6. A driver that sets them expects its writes to
		// be dropped, and drawing them anyway would show a shape the chip is not playing.
		if ((impl->test & 0x40) != 0 || (! impl->modePlus && (impl->test & 0x80) != 0
										 && offset >= kSharedTableStart))
		{
			break;
		}

		if (impl->modePlus)
		{
			const int channel = offset >> 5;	// 0x00-0x9F: channels 0-4, no sharing

			if (channel < numChannels)
			{
				impl->waveram[channel][step] = (int8_t) value;
			}
		}
		else if (offset >= kSharedTableStart)
		{
			// SCC shares one table between channels 3 and 4, so one write lands in both.
			impl->waveram[3][step] = (int8_t) value;
			impl->waveram[4][step] = (int8_t) value;
		}
		else
		{
			const int channel = offset >> 5;	// 0x00-0x5F: channels 0-2

			impl->waveram[channel][step] = (int8_t) value;
		}

		break;
	}
	case 0x01:
	{
		const int channel = (impl->latch >> 1) & 0x07;
		if (channel >= numChannels)
		{
			break;
		}

		if ((impl->latch & 1) != 0)
		{
			impl->frequency[channel] = (impl->frequency[channel] & 0x0FF) | ((value & 0x0F) << 8);
		}
		else
		{
			impl->frequency[channel] = (impl->frequency[channel] & 0xF00) | value;
		}

		break;
	}
	case 0x02:
	{
		const int channel = impl->latch & 0x07;
		if (channel < numChannels)
		{
			impl->volume[channel] = (uint8_t) (value & 0x0F);
		}

		break;
	}
	case 0x03:
		// the key mask is the data byte; the address only carries the latch
		for (int channel = 0; channel < numChannels; ++channel)
		{
			impl->key[channel] = (value & (1 << channel)) != 0;
		}
		break;
	case 0x05:
		impl->test = value;
		break;
	default:
		break;
	}
}

void K051649Decoder::advance (uint32_t sampleTime)
{
	(void) sampleTime;		// no timers
}

ChannelState K051649Decoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= channelCount_)
	{
		return state;
	}

	state.present = true;
	state.period = (uint32_t) impl->frequency[channel];
	state.volume = impl->volume[channel];
	state.gate = impl->key[channel] && state.volume > 0
				 && impl->frequency[channel] > kMinimumFrequency;
	state.patch = 0;

	// **The row draws the table the channel walks**, one cycle by definition, at the rate the
	// counter steps it. One step lasts `(f + 1) / 2` clocks (the core's `step` over its 16.16
	// accumulator), so the step rate is `2 * clock / (f + 1)` and the tone is that over thirty-two -
	// the same `clock / (16 * (f + 1))` the note table uses, which keeps note and picture in step.
	state.wave = impl->waveFor (channel);
	state.tableWave = true;
	state.sampleRate = (float) (2.0 * impl->clock / ((double) impl->frequency[channel] + 1.0));

	return state;
}

const NoteTable& K051649Decoder::noteTable (int channel) const
{
	(void) channel;		// one divider for all five channels
	return *impl->table;
}

ChannelKind K051649Decoder::channelKind (int channel) const
{
	(void) channel;
	return ChannelKind::sccWave;
}

} // namespace toybox::chips
