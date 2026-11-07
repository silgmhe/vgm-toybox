#include "Msm5232Decoder.h"

#include <cmath>
#include <cstdint>
#include <map>

#include "Msm5232Shape.h"
#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	/** The ROM the register indexes has eighty-eight entries - A0 upwards (msm5232.c:10). */
	constexpr int kFirstKey = 21;

	/** A value this high does not name a pitch: it puts the voice into noise mode (msm5232.c:290). */
	constexpr uint8_t kNoiseThreshold = 0xD8;

	/** A note frequency from the keyboard index. The index *is* the key, so this is not a fit. */
	double hzOf (int index)
	{
		const int note = kFirstKey + index;

		return 440.0 * std::pow (2.0, (double) (note - 69) / 12.0);
	}

	/** The group control register's low nibble enables the four output taps (msm5232.c:620-650). */
	int groupOf (int channel) { return (channel >> 2) & 1; }
}

struct Msm5232Decoder::Impl
{
	struct Voice
	{
		int key = 0;			// index into the eighty-eight-entry ROM
		bool keyed = false;
		bool noise = false;
	};

	/** The index *is* the note, so the table has nothing to compute and nothing to measure. */
	struct Table final : NoteTable
	{
		int noteFor (uint32_t period, int, int* finePitch) const override
		{
			if (finePitch != nullptr)
			{
				*finePitch = 0;
			}

			const int index = (int) (period > (uint32_t) msm5232::kLastIndex
										 ? msm5232::kLastIndex
										 : period);
			return kFirstKey + index;
		}
	};

	Table table;
	Voice voices[8];

	/** `0x0C` and `0x0D`: which of the four taps each group of four voices feeds. */
	int groupControl[2] = { 0, 0 };

	/** One shape per (divider, enable mask): both are small, and the row asks every frame. */
	mutable std::map<uint64_t, std::shared_ptr<const std::vector<float>>> shapes;

	std::shared_ptr<const std::vector<float>> shapeFor (int divider, int enabled) const
	{
		const uint64_t key = ((uint64_t) (divider & 0xFF) << 8) | (uint64_t) (enabled & 0xFF);
		const auto cached = shapes.find (key);

		if (cached != shapes.end())
		{
			return cached->second;
		}

		auto points = std::make_shared<std::vector<float>> ();

		if (msm5232::buildShape (divider, enabled, *points) < 2)
		{
			shapes.emplace (key, nullptr);
			return nullptr;
		}

		shapes.emplace (key, points);
		return points;
	}
};

Msm5232Decoder::Msm5232Decoder() = default;
Msm5232Decoder::~Msm5232Decoder() = default;

void Msm5232Decoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	impl = std::make_unique<Impl>();
}

void Msm5232Decoder::write (const RegisterWrite& write)
{
	if (impl == nullptr)
	{
		return;
	}

	// **Above 0x0B the registers stop being per-voice**, and the two that matter here are the group
	// controls: their low nibble says which of the four output taps the group of four even feeds
	// (`msm5232.c:620-650`). A voice whose group enables nothing is inaudible however it is keyed.
	if (write.reg >= 0x08)
	{
		if (write.reg == 0x0C || write.reg == 0x0D)
		{
			impl->groupControl[write.reg & 1] = write.value & 0x0F;
		}

		return;
	}

	Impl::Voice& voice = impl->voices[write.reg & 0x07];

	// Bit 7 is the gate; the rest is the key index, unless the value is high enough to mean noise.
	voice.keyed = (write.value & 0x80) != 0;
	voice.noise = write.value >= kNoiseThreshold;

	if (! voice.noise)
	{
		voice.key = write.value & 0x7F;

		if (voice.key > msm5232::kLastIndex)
		{
			voice.key = msm5232::kLastIndex;
		}
	}
}

void Msm5232Decoder::advance (uint32_t)
{
	// the envelope is the chip's own business; every boundary here is a write
}

ChannelState Msm5232Decoder::channelState (int channel) const
{
	ChannelState state;
	if (impl == nullptr || channel < 0 || channel >= 8)
	{
		return state;
	}

	const Impl::Voice& voice = impl->voices[channel];
	const int enabled = impl->groupControl[groupOf (channel)];

	state.present = true;
	state.gate = voice.keyed && (voice.noise || enabled != 0);
	state.volume = state.gate ? 15 : 0;
	state.period = (uint32_t) voice.key;

	if (! state.gate)
	{
		return state;
	}

	if (voice.noise)
	{
		state.hz = 0.0;
		state.pitchSource = PitchSource::unpitched;
		return state;
	}

	state.hz = hzOf (voice.key);
	state.pitchSource = PitchSource::registerExact;

	// **What the voice emits is the sum of its group's enabled taps**, not a plain square: four
	// fifty-percent squares taken from one counter, phase-aligned and octave apart. One period is
	// `2^(divider + 1)` ticks, and the rate is how many of those make a note.
	state.wave = impl->shapeFor (msm5232::dividerOf (voice.key), enabled);

	if (state.wave != nullptr && state.wave->size () >= 2)
	{
		state.sampleRate = (float) (state.hz * (double) state.wave->size ());
		state.tableWave = true;
	}

	return state;
}

const NoteTable& Msm5232Decoder::noteTable (int) const
{
	static const Impl::Table fallback;
	return impl != nullptr ? impl->table : fallback;
}

ChannelKind Msm5232Decoder::channelKind (int channel) const
{
	if (impl == nullptr || channel < 0 || channel >= 8)
	{
		return ChannelKind::sn76489Square;
	}

	// A staircase is a shape the row draws from `wave`, not a pulse it has to synthesise - the
	// same reasoning as Mikey's cycle, and the opposite of a plain square with a duty.
	if (impl->voices[channel].noise)
	{
		return ChannelKind::ayNoise;
	}

	return impl->shapeFor (msm5232::dividerOf (impl->voices[channel].key),
						   impl->groupControl[groupOf (channel)]) != nullptr
			   ? ChannelKind::wavetableWave
			   : ChannelKind::sn76489Square;
}

} // namespace toybox::chips
