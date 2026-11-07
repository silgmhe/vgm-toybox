#include "VsuDecoder.h"

#include <cstdint>
#include <map>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	/** Thirty-two steps make one cycle of the wave RAM. */
	constexpr double kStepsPerCycle = 32.0;

	/** `2048 - freq` is the reload value, so the range of the divider is 1..2048. */
	constexpr double kDividerRange = 2048.0;

	/** The clock to assume when the VGM header carries none for this chip. The songs here declare
		five million (`vsu.c` runs the counters off the CPU clock), so this is only a fallback. */
	constexpr uint32_t kDefaultClock = 5370000;

	/** The sixth voice is an LFSR noise source, not a wavetable voice. */
	constexpr int kNoiseChannel = 5;

	/** Five tables of thirty-two six-bit entries - `vsu.c:116`. The sixth address value plays
		silence (`vsu.c:383`), which is why the table index is checked before it is used. */
	constexpr int kTables = 5;
	constexpr int kTableBytes = 32;

	/** Six bits, silence at 0x20: `vsu.c:402` subtracts it before mixing. */
	constexpr int kWaveBias = 0x20;

	double frequencyOf (uint32_t clock, uint16_t freq)
	{
		const double remaining = kDividerRange - (double) (freq & 0x7FF);
		if (remaining <= 0.0)
		{
			return 0.0;
		}

		return (double) clock / (kStepsPerCycle * remaining);
	}

	/** The rate a one-cycle file of the table has to declare: how many entries the voice walks per
		second. A wave cycle is thirty-two of them, which is the frequency above times 32. */
	double stepRateOf (uint32_t clock, uint16_t freq)
	{
		const double remaining = kDividerRange - (double) (freq & 0x7FF);
		if (remaining <= 0.0)
		{
			return 0.0;
		}

		return (double) clock / remaining;
	}
}

struct VsuDecoder::Impl
{
	struct Voice
	{
		uint16_t freq = 0;			// eleven bits: low byte at 0x2, top three at 0x3
		bool keyed = false;
		int ramAddress = 0;			// register 0x6 - which of the five tables
		int envelope = 0;
	};

	/** All six voices share one formula, so one table serves them. */
	struct Table final : NoteTable
	{
		uint32_t clock = kDefaultClock;

		int noteFor (uint32_t period, int, int* finePitch) const override
		{
			const double hz = frequencyOf (clock, (uint16_t) period);
			if (hz <= 0.0 || hz > 4186.0)
			{
				// past MIDI's ceiling - a noise-rate divider, reported as a trigger rather than dropped
				if (finePitch != nullptr)
				{
					*finePitch = 0;
				}

				return 60;
			}

			return quantiseFrequency (hz, finePitch);
		}
	};

	Table table;
	Voice voices[6];

	/** **The wave RAM, which is the whole reason this chip has anything to draw.**
		`vsu.c:116` keeps five tables of thirty-two entries, and they arrive as ordinary writes to
		the low half of the address space (`vsu.c:229`) - not as a ROM, so there is no memory
		requirement and no data block to assemble. A driver uploads them before it enables anything:
		the core refuses wave writes while any voice is on (`vsu.c:222-228`), which is why the
		dumped songs show a few hundred writes before the first register one.
	*/
	uint8_t waveRam[kTables][kTableBytes] = {};

	/** Decoded tables, keyed by their contents. A table can be rewritten between songs' sections,
		so the cache has to notice a change - a hash of the thirty-two bytes does that, and a stale
		pointer would draw the previous shape. */
	mutable std::map<uint64_t, std::shared_ptr<const std::vector<float>>> waveCache;

	uint64_t hashOf (int table) const
	{
		uint64_t hash = 1469598103934665603ull;

		for (int i = 0; i < kTableBytes; ++i)
		{
			hash ^= (uint64_t) waveRam[table][i];
			hash *= 1099511628211ull;
		}

		return hash;
	}

	/** One cycle of a table, decoded to -1..1. */
	std::shared_ptr<const std::vector<float>> waveFor (int table) const
	{
		const uint64_t key = hashOf (table);
		const auto cached = waveCache.find (key);

		if (cached != waveCache.end())
		{
			return cached->second;
		}

		auto wave = std::make_shared<std::vector<float>> (kTableBytes);

		for (int i = 0; i < kTableBytes; ++i)
		{
			(*wave)[(std::size_t) i] = (float) ((int) waveRam[table][i] - kWaveBias) / 32.0f;
		}

		waveCache.emplace (key, wave);
		return wave;
	}
};

VsuDecoder::VsuDecoder() = default;
VsuDecoder::~VsuDecoder() = default;

void VsuDecoder::reset (const ChipConfig& config)
{
	const uint32_t clock = (config.clockHz != 0) ? config.clockHz : kDefaultClock;

	impl = std::make_unique<Impl>();
	impl->table.clock = clock;
}

void VsuDecoder::write (const RegisterWrite& write)
{
	if (impl == nullptr)
	{
		return;
	}

	// **Two address spaces share one low byte.** `port` says which half this write is in: the
	// loader sets it from the VGM offset's bit 8, because the wave RAM lives below 0x100 and the
	// channel registers at 0x100..0x17F. Without the split, a wave byte written at 0x10 read as
	// channel 1's control register, and a wave upload could key a voice on.
	if (write.port == 0)
	{
		if (write.reg < (kTables * kTableBytes))
		{
			impl->waveRam[write.reg / kTableBytes][write.reg % kTableBytes] =
				(uint8_t) (write.value & 0x3F);
		}

		return;		// 0xA0..0xFF is the modulation table; no row draws it
	}

	const int voice = (write.reg >> 4) & 0x0F;
	if (voice > 5)
	{
		return;
	}

	const int reg = write.reg & 0x0F;
	Impl::Voice& state = impl->voices[voice];

	switch (reg)
	{
	case 0x0:		// IntlControl: bit 7 keys the voice on; the low five bits are the interval
		state.keyed = (write.value & 0x80) != 0;
		break;
	case 0x2:		// frequency, low eight bits
		state.freq = (uint16_t) ((state.freq & 0x700) | write.value);
		break;
	case 0x3:		// frequency, high three bits
		state.freq = (uint16_t) ((state.freq & 0x0FF) | ((write.value & 0x07) << 8));
		break;
	case 0x4:		// envelope control - level, not pitch
		state.envelope = write.value;
		break;
	case 0x6:		// **the wave table index** (`vsu.c:358`), not 0x5 - that one is EnvControl's
		state.ramAddress = (int) (write.value & 0x0F);
		break;
	default:
		break;		// 0x1 sweep, 0x5 EnvControl, 0x7 sweep control, 0x8..0xF level and interval
	}
}

void VsuDecoder::advance (uint32_t)
{
	// every boundary comes from a write; there is no envelope to step
}

ChannelState VsuDecoder::channelState (int channel) const
{
	ChannelState state;
	if (impl == nullptr || channel < 0 || channel >= 6)
	{
		return state;
	}

	const Impl::Voice& voice = impl->voices[channel];
	state.present = true;
	state.gate = voice.keyed && voice.freq > 0;
	state.volume = state.gate ? 15 : 0;
	state.patch = voice.ramAddress;
	state.period = voice.freq;

	if (! state.gate)
	{
		return state;
	}

	if (channel == kNoiseChannel)
	{
		// an LFSR source: it sounds, but what it produces has no period to name
		state.hz = 0.0;
		state.pitchSource = PitchSource::unpitched;
		return state;
	}

	state.hz = frequencyOf (impl->table.clock, voice.freq);
	state.pitchSource = PitchSource::registerExact;

	// **The shape and the rate a file of it needs.** One cycle of the table the voice points at,
	// and the number of entries it walks per second - so a dragged file is exactly one period at
	// the pitch the row shows.
	if (voice.ramAddress < kTables)
	{
		state.wave = impl->waveFor (voice.ramAddress);
		state.tableWave = true;		// one cycle by definition, whatever rate it is walked at
	}

	state.sampleRate = (float) stepRateOf (impl->table.clock, voice.freq);

	return state;
}

const NoteTable& VsuDecoder::noteTable (int) const
{
	static const Impl::Table fallback;
	return impl != nullptr ? impl->table : fallback;
}

ChannelKind VsuDecoder::channelKind (int channel) const
{
	return channel == kNoiseChannel ? ChannelKind::ayNoise : ChannelKind::wavetableWave;
}

} // namespace toybox::chips
