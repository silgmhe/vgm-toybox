#include "K005289Decoder.h"

#include <cstdint>
#include <map>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	/** Thirty-two steps make one cycle of the PROM (`addr & 0x1F`, k005289.c:191). */
	constexpr double kStepsPerCycle = 32.0;

	/** The clock to assume when the VGM header carries none for this chip. */
	constexpr uint32_t kDefaultClock = 3579545;

	/** `2 * 0x100`: one 256-byte half per channel - eight waveforms of thirty-two steps
		(`k005289.c:59`, `195-197`). */
	constexpr int kPromBytes = 0x200;
	constexpr int kHalfBytes = 0x100;
	constexpr int kWaveformBytes = 32;
	constexpr int kWaveforms = 8;

	/** Four bits with `- 8` on the way out (`k005289.c:198`): -8..+7, so the scale is eight. */
	constexpr int kWaveBias = 8;
	constexpr float kWaveScale = 8.0f;

	double frequencyOf (uint32_t clock, uint16_t divider)
	{
		return (double) clock / (kStepsPerCycle * ((double) divider + 1.0));
	}

	/** How many PROM entries a voice walks per second - the rate a one-cycle file declares. */
	double stepRateOf (uint32_t clock, uint16_t divider)
	{
		return (double) clock / ((double) divider + 1.0);
	}
}

struct K005289Decoder::Impl
{
	struct Voice
	{
		uint16_t latch = 0;		// the LD value, already inverted by the driver
		uint16_t freq = 0;			// what TG copied out of it - the pitch in force
		bool triggered = false;
		int volume = 0;					// 0..15; zero is silence
		int waveform = 0;
	};

	/** Both voices share one formula, so one table serves them: it needs the clock, nothing else. */
	struct Table final : NoteTable
	{
		uint32_t clock = kDefaultClock;

		int noteFor (uint32_t period, int, int* finePitch) const override
		{
			return quantiseFrequency (frequencyOf (clock, (uint16_t) period), finePitch);
		}
	};

	Table table;
	Voice voices[2];

	/** The song's PROM: 512 four-bit entries, or empty when the file carried none. */
	std::vector<uint8_t> prom;

	/** Decoded waveforms, keyed by their contents. A table is static once loaded, but the cache
		makes a per-frame `channelState` allocation-free in the common case. */
	mutable std::map<uint64_t, std::shared_ptr<const std::vector<float>>> waveCache;

	bool hasProm() const { return prom.size () >= (std::size_t) kPromBytes; }

	uint64_t hashOf (int channel, int waveform) const
	{
		uint64_t hash = 1469598103934665603ull;
		const std::size_t base = (std::size_t) (channel * kHalfBytes + waveform * kWaveformBytes);

		for (int i = 0; i < kWaveformBytes; ++i)
		{
			hash ^= (uint64_t) prom[base + (std::size_t) i];
			hash *= 1099511628211ull;
		}

		return hash;
	}

	std::shared_ptr<const std::vector<float>> waveFor (int channel, int waveform) const
	{
		if (! hasProm () || waveform < 0 || waveform >= kWaveforms)
		{
			return nullptr;
		}

		const uint64_t key = hashOf (channel, waveform);
		const auto cached = waveCache.find (key);

		if (cached != waveCache.end())
		{
			return cached->second;
		}

		auto wave = std::make_shared<std::vector<float>> (kWaveformBytes);
		const std::size_t base = (std::size_t) (channel * kHalfBytes + waveform * kWaveformBytes);

		for (int i = 0; i < kWaveformBytes; ++i)
		{
			(*wave)[(std::size_t) i] =
				(float) ((int) prom[base + (std::size_t) i] - kWaveBias) / kWaveScale;
		}

		waveCache.emplace (key, wave);
		return wave;
	}
};

K005289Decoder::K005289Decoder() = default;
K005289Decoder::~K005289Decoder() = default;

void K005289Decoder::reset (const ChipConfig& config)
{
	const uint32_t clock = (config.clockHz != 0) ? config.clockHz : kDefaultClock;

	// The PROM belongs to the song, not to the run: a rewind must not lose the image the file
	// already supplied.
	std::vector<uint8_t> prom;

	if (impl != nullptr && impl->hasProm ())
	{
		prom = impl->prom;
	}

	impl = std::make_unique<Impl>();
	impl->table.clock = clock;
	impl->prom = std::move (prom);
}

void K005289Decoder::setProm (std::vector<uint8_t> prom)
{
	if (impl == nullptr)
	{
		impl = std::make_unique<Impl>();
	}

	prom.resize ((std::size_t) kPromBytes, 0);
	impl->prom = std::move (prom);
	impl->waveCache.clear ();
}

void K005289Decoder::write (const RegisterWrite& write)
{
	if (impl == nullptr)
	{
		return;
	}

	const int channel = write.reg & 1;
	Impl::Voice& voice = impl->voices[channel];

	// the reader packs the command's twelve bits into one word (ChipTypes.h)
	const uint16_t data = (uint16_t) (write.value & 0x0FFF);

	switch (write.reg & 0x06)
	{
	case 0x00:		// control: volume in the low nibble, waveform number in bits 5-7
		voice.volume = data & 0x0F;
		voice.waveform = (data >> 5) & 0x07;
		break;
	case 0x02:		// LD: the driver writes the inverted divider, the core stores 0xFFF - value
		voice.latch = (uint16_t) (0x0FFF - (data & 0x0FFF));
		break;
	case 0x04:		// TG: the latched pitch becomes audible - the key-on edge
		voice.freq = voice.latch;
		voice.triggered = true;
		break;
	default:
		break;
	}
}

void K005289Decoder::advance (uint32_t)
{
	// note boundaries come from the writes; there is no envelope to step here
}

ChannelState K005289Decoder::channelState (int channel) const
{
	ChannelState state;
	if (impl == nullptr || channel < 0 || channel >= 2)
	{
		return state;
	}

	const Impl::Voice& voice = impl->voices[channel];
	state.present = true;
	state.gate = voice.triggered && voice.volume > 0;
	state.volume = voice.volume;
	state.patch = voice.waveform;
	state.period = voice.freq;

	if (! state.gate)
	{
		return state;
	}

	state.hz = frequencyOf (impl->table.clock, voice.freq);
	state.pitchSource = PitchSource::registerExact;

	// **The shape and the rate a file of it needs.** One cycle of the waveform the control register
	// names, and the entry rate - thirty-two of them make the cycle. A song without a `0xC3` block
	// still sounds, so the row keeps its pitch and only the shape is missing.
	state.wave = impl->waveFor (channel, voice.waveform);

	if (state.wave != nullptr)
	{
		state.tableWave = true;
		state.sampleRate = (float) stepRateOf (impl->table.clock, voice.freq);
	}

	return state;
}

const NoteTable& K005289Decoder::noteTable (int) const
{
	static const Impl::Table fallback;
	return impl != nullptr ? impl->table : fallback;
}

ChannelKind K005289Decoder::channelKind (int) const
{
	return ChannelKind::wavetableWave;
}

} // namespace toybox::chips
