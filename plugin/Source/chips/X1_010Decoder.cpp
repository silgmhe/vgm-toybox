#include "X1_010Decoder.h"

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
	// Seta boards run the X1-010 at 16 MHz; the header clock is used when it is there.
	constexpr uint32_t kDefaultClock = 16000000;

	constexpr int kOneShotNote = 60;
	constexpr uint32_t kOneShotPeriod = 0x80000000u;

	/** The PCM sample ROM, as VGM names it in a `0x67` block, and the address space it fills. */
	constexpr uint8_t kRomType = 0x91;
	constexpr std::size_t kRomBytes = 1u << 20;

	/** Waveform RAM (`x1_010.c:44-46`): thirty-two tables of 128 signed bytes, at 0x1000. */
	constexpr int kWaveCount = 32;
	constexpr int kWaveSteps = 128;
	constexpr uint32_t kWaveRamBase = 0x1000;
	constexpr uint32_t kWaveRamBytes = (uint32_t) kWaveCount * (uint32_t) kWaveSteps;
	constexpr uint32_t kWaveRamEnd = kWaveRamBase + kWaveRamBytes;

	/** A cache with a bound rather than a container that grows: a row is asked once per frame and a
		driver may switch samples every frame. */
	constexpr std::size_t kCacheLimit = 64;

	/** **A display guard, not a knob.** A PCM sample longer than this is decoded only as far as
		this; the row paints 128 columns and the one 1 MB register pair in the test set would
		otherwise cost 4 MB of floats per channel. The exported file has its own length limit
		(`settings.export.sampleSeconds`), which is a preference and belongs to the writer. */
	constexpr std::size_t kMaxPcmPoints = 262144;

	/** The core substitutes 4 for a zero frequency register (`x1_010.c:231`), a hack for a driver
		that never writes it; without it the rate would be zero and the gate endless. */
	constexpr int kMinimumPcmFrequency = 4;

	// One wavetable cycle is 128 bytes and the core steps the table clock * pitch / 2^26 times a
	// second (x1_010.c: step = base_clock / 524288 * freq, stepped through 128 samples).
	constexpr double kCycleDivisor = 67108864.0;
	constexpr double kStepDivisor = kCycleDivisor / (double) kWaveSteps;	// 524288

	/** FNV-1a over one table's bytes. */
	uint64_t hashOfTable (const int8_t* bytes)
	{
		uint64_t hash = 1469598103934665603ull;

		for (int i = 0; i < kWaveSteps; ++i)
		{
			hash = (hash ^ (uint64_t) (uint8_t) bytes[i]) * 1099511628211ull;
		}

		return hash;
	}

	class X1NoteTable final : public NoteTable
	{
	public:
		explicit X1NoteTable (double clock) : clock (clock) {}

		int noteFor (uint32_t period, int, int* finePitch) const override
		{
			if ((period & kOneShotPeriod) != 0)
			{
				if (finePitch != nullptr)
				{
					*finePitch = 0;
				}

				return kOneShotNote;
			}

			return quantiseFrequency (clock * (double) period / kCycleDivisor, finePitch);
		}

	private:
		double clock;
	};
}

struct X1_010Decoder::Impl
{
	struct Channel
	{
		uint8_t status = 0;	// 0: bit 7 divider, bit 2 repeat, bit 1 waveform, bit 0 key on
		uint8_t volume = 0;	// 1: PCM L/R nibbles, or the waveform number in waveform mode
		uint8_t pitchLo = 0;	// 2: PCM frequency, or the low half of the wave pitch
		uint8_t pitchHi = 0;	// 3: the high half of the wave pitch
		uint8_t startReg = 0;	// 4: PCM start / 0x1000, or the envelope time
		uint8_t endReg = 0;		// 5: 0x100 - (PCM end / 0x1000), or the envelope number
		uint32_t gateEnd = 0;	// PCM: the sample's own duration closes the gate here
		bool gateOpen = false;
	};

	Channel channels[numChannels];
	uint32_t now = 0;
	double clock = (double) kDefaultClock;
	double tickRate = 44100.0;		// the unit `RegisterWrite::sampleTime` arrives in

	/** The PCM sample ROM, or empty when the song carries none. */
	std::vector<uint8_t> rom;

	/** **The waveform RAM**, thirty-two tables the driver uploads once at the start. It lives in
		the chip's own register space rather than in a ROM block (`x1_010.c:44-46`). */
	int8_t waveRam[kWaveCount][kWaveSteps] = {};

	/** Decoded shapes. Two maps because the two keys mean different things - a table's content and
		a sample's `(start, length)` - and mixing them would risk a collision. */
	mutable std::map<uint64_t, std::shared_ptr<const std::vector<float>>> waveCache;
	mutable std::map<uint64_t, std::shared_ptr<const std::vector<float>>> sampleCache;

	std::unique_ptr<X1NoteTable> table;

	bool isWaveform (int channel) const { return (channels[channel].status & 0x02) != 0; }
	bool isKeyedOn (int channel) const { return (channels[channel].status & 0x01) != 0; }

	/** The divider flag halves the frequency as well as the pitch (`x1_010.c:222`, `:257`). */
	int divOf (int channel) const { return (channels[channel].status & 0x80) != 0 ? 1 : 0; }

	/** 14-bit 6.10 pitch, halved by the divider flag - the same shift the core applies. */
	uint32_t pitchOf (int channel) const
	{
		const Channel& current = channels[channel];
		uint32_t pitch = ((uint32_t) current.pitchHi << 8) | current.pitchLo;

		if ((current.status & 0x80) != 0)
		{
			pitch >>= 1;
		}

		return pitch;
	}

	/** The sample's first byte, in a 1 MB space addressed in 4 KB pages (`x1_010.c:224`). */
	uint32_t pcmStart (int channel) const
	{
		return (uint32_t) channels[channel].startReg << 12;
	}

	/** The first byte after it: the register counts backwards from 0x100 pages (`x1_010.c:225`). */
	uint32_t pcmEnd (int channel) const
	{
		return (uint32_t) (0x100 - channels[channel].endReg) << 12;
	}

	/** **The playback rate of a PCM sample**: `clock / 8192` per frequency step (`x1_010.c:233`). */
	double pcmRate (int channel) const
	{
		const int freq = std::max ((int) (channels[channel].pitchLo >> divOf (channel)),
								   kMinimumPcmFrequency);

		return clock / 8192.0 * (double) freq;
	}

	/** **The step rate of a wavetable**: `clock * pitch / 524288`, one cycle over 128 steps. */
	double waveStepRate (int channel) const
	{
		return clock * (double) pitchOf (channel) / kStepDivisor;
	}

	/** The decoded table a waveform channel is walking, cached by the table's own bytes. */
	std::shared_ptr<const std::vector<float>> waveTable (int channel) const
	{
		const int number = channels[channel].volume % kWaveCount;
		const uint64_t key = hashOfTable (waveRam[number]);

		const auto found = waveCache.find (key);

		if (found != waveCache.end ())
		{
			return found->second;
		}

		auto wave = std::make_shared<std::vector<float>> ();
		wave->reserve (kWaveSteps);

		for (int i = 0; i < kWaveSteps; ++i)
		{
			wave->push_back ((float) waveRam[number][i] / 128.0f);
		}

		if (waveCache.size () >= kCacheLimit)
		{
			waveCache.clear ();
		}

		return waveCache.emplace (key, std::move (wave)).first->second;
	}

	/** The decoded bytes a PCM channel is playing, cached by `(start, length)` like every other
		sampled chip. Null when the registers name nothing in the ROM. */
	std::shared_ptr<const std::vector<float>> pcmSample (int channel) const
	{
		const uint32_t start = pcmStart (channel);
		const uint32_t end = pcmEnd (channel);

		if (rom.empty () || end <= start || start >= rom.size ())
		{
			return nullptr;
		}

		const std::size_t length = (std::min<std::size_t>) (end - start, rom.size () - start);

		if (length < 2)
		{
			return nullptr;
		}

		const uint64_t key = ((uint64_t) start << 32) | (uint64_t) length;
		const auto found = sampleCache.find (key);

		if (found != sampleCache.end ())
		{
			return found->second;
		}

		const std::size_t count = (std::min) (length, kMaxPcmPoints);
		auto wave = std::make_shared<std::vector<float>> ();
		wave->reserve (count);

		for (std::size_t i = 0; i < count; ++i)
		{
			wave->push_back ((float) (int8_t) rom[start + i] / 128.0f);
		}

		if (sampleCache.size () >= kCacheLimit)
		{
			sampleCache.clear ();
		}

		return sampleCache.emplace (key, std::move (wave)).first->second;
	}
};

X1_010Decoder::X1_010Decoder() : impl (std::make_unique<Impl>())
{
	reset (ChipConfig { kDefaultClock, 44100.0 });
}

X1_010Decoder::~X1_010Decoder() = default;

void X1_010Decoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const uint32_t clock = config.clockHz;

	impl = std::make_unique<Impl>();
	impl->clock = (clock > 100000) ? (double) clock : (double) kDefaultClock;

	// The gate is a duration in the same unit the writes arrive in, so the two have to agree: the
	// player feeds 44.1 kHz sample times, but a module loader could hand this decoder another unit.
	impl->tickRate = (config.timeBaseHz > 0.0) ? config.timeBaseHz : 44100.0;
	impl->table = std::make_unique<X1NoteTable> (impl->clock);
}

void X1_010Decoder::attachMemory (const ChipMemory& memory)
{
	if (memory.bytes != nullptr && ! memory.bytes->empty ())
	{
		impl->rom = *memory.bytes;
	}
}

void X1_010Decoder::write (const RegisterWrite& w)
{
	// The VGM command carries a 16-bit offset; the channel registers are the low 0x80 of it.
	const int reg = ((int) w.port << 8) | (int) w.reg;

	// **Waveform RAM is written through the same command**, above the channel and envelope blocks:
	// 0x1000-0x1FFF is thirty-two tables of 128 signed bytes (`x1_010.c:44-46`).
	if (reg >= (int) kWaveRamBase && reg < (int) kWaveRamEnd)
	{
		const uint32_t offset = (uint32_t) (reg - (int) kWaveRamBase);
		impl->waveRam[offset / kWaveSteps][offset % kWaveSteps] = (int8_t) w.value;
		return;
	}

	// Envelope RAM (0x80-0xFFF) shapes the volume, not the shape; nothing here draws it.
	if (reg < 0 || reg >= 0x80)
	{
		return;
	}

	const int channel = reg >> 3;
	const int field = reg & 0x07;
	Impl::Channel& current = impl->channels[channel];

	switch (field)
	{
	case 0:
	{
		const bool wasOn = (current.status & 0x01) != 0;
		current.status = (uint8_t) w.value;

		const bool isOn = (current.status & 0x01) != 0;
		const bool waveform = (current.status & 0x02) != 0;

		if (! isOn)
		{
			current.gateOpen = false;
		}
		else if (! wasOn && ! waveform)
		{
			// **The registers do say where the sample ends.** Start and end are both here, and the
			// rate is the frequency register over the chip's own time base, so the window is the
			// sample's real duration - the core keys the channel off at the end address for the
			// same reason (`x1_010.c:240-242`), and a fixed window used to guess at it.
			const uint32_t start = impl->pcmStart (channel);
			const uint32_t end = impl->pcmEnd (channel);
			const double rate = impl->pcmRate (channel);
			const double seconds = (rate > 0.0 && end > start)
					? (double) (end - start) / rate : 0.0;

			if (seconds > 0.0)
			{
				current.gateEnd = (uint32_t) (w.sampleTime + seconds * impl->tickRate);
				current.gateOpen = true;
			}
			else
			{
				current.gateOpen = false;		// an empty range: the core keys it off at once
			}
		}

		break;
	}
	case 1:
		current.volume = (uint8_t) w.value;
		break;
	case 2:
		current.pitchLo = (uint8_t) w.value;
		break;
	case 3:
		current.pitchHi = (uint8_t) w.value;
		break;
	case 4:
		current.startReg = (uint8_t) w.value;
		break;
	case 5:
		current.endReg = (uint8_t) w.value;
		break;
	default:
		break;		// 6/7 reserved
	}
}

void X1_010Decoder::advance (uint32_t sampleTime)
{
	impl->now = sampleTime;

	for (auto& channel : impl->channels)
	{
		if (channel.gateOpen && sampleTime >= channel.gateEnd)
		{
			channel.gateOpen = false;
		}
	}
}

ChannelState X1_010Decoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= channelCount_)
	{
		return state;
	}

	const bool waveform = impl->isWaveform (channel);
	state.present = true;
	state.patch = impl->channels[channel].volume;

	if (waveform)
	{
		state.gate = impl->isKeyedOn (channel);
		state.period = impl->pitchOf (channel);
		state.volume = 15;		// the envelope registers shape the volume; not modelled

		// **A table, not a recording.** One cycle is the 128 steps the channel walks, at the rate
		// the counter steps them; `tableWave` is what sends the export down the cycle path.
		state.wave = impl->waveTable (channel);
		state.tableWave = true;
		state.sampleRate = (float) impl->waveStepRate (channel);
	}
	else
	{
		state.gate = impl->channels[channel].gateOpen;
		state.period = kOneShotPeriod;
		state.volume = 15;

		// **A recording**: the ROM bytes between start and end, at the rate the frequency register
		// sets - `tableWave` stays false, so the export writes a sample at that rate.
		state.wave = impl->pcmSample (channel);
		state.sampleRate = (float) impl->pcmRate (channel);
	}

	return state;
}

const NoteTable& X1_010Decoder::noteTable (int channel) const
{
	(void) channel;		// one table: PCM hands it the one-shot marker, waveform a real pitch
	return *impl->table;
}

ChannelKind X1_010Decoder::channelKind (int channel) const
{
	return impl->isWaveform (channel) ? ChannelKind::x1Wave : ChannelKind::adpcmDrum;
}

bool X1_010Decoder::isDrum (int channel) const
{
	return ! impl->isWaveform (channel);
}

} // namespace toybox::chips
