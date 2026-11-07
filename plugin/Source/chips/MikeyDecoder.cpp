#include "MikeyDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <map>

#include "MikeyPattern.h"
#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	constexpr int kChannels = 4;

	/** The Lynx runs MIKEY from the 16 MHz system clock. */
	constexpr uint32_t kDefaultClock = 16000000;

	/** `2^(shift + 4)` master clocks per timer tick - mikey.c:355. */
	constexpr int kTicksShift = 4;

	/** Steps per second: one timer tick each. */
	double stepRateOf (uint32_t clock, int backup, int shift)
	{
		const double ticks = std::pow (2.0, (double) (shift + kTicksShift));

		return (double) clock / ((double) (backup + 1) * ticks);
	}
}

struct MikeyDecoder::Impl
{
	struct Voice
	{
		int volume = 0;				// signed eight-bit, `mikey.c:414`
		int backup = 0;				// the timer's reload value
		int shift = 0;				// bits 0-2 of CTRLA
		bool integrate = false;		// CTRLA bit 5 - accumulate instead of switching
		uint16_t shiftReg = 0;		// twelve bits: SHIFT plus the top four of OTHER
		uint16_t taps = 0;			// nine bits: FEEDBACK plus CTRLA bit 7
	};

	struct Table final : NoteTable
	{
		uint32_t clock = kDefaultClock;

		int noteFor (uint32_t, int, int* finePitch) const override
		{
			// **This chip has no note to give from a divider alone.** Every other backend can turn a
			// period into a frequency because the divider is the frequency; here the divider is the
			// *step* rate and the fundamental is that divided by the LFSR cycle's length - two steps
			// for a plain tone, four for the taps these drivers pick, thousands for noise. A table
			// that assumed two (which this one did) invented a note: a silent channel whose period
			// was left behind reported 3937 Hz / note 107 on every frame, because the fallback read
			// the step divider as if it were a pitch divider.
			//
			// `channelState` knows the cycle and sets `hz` from it, so this is only reached when
			// there is no cycle - noise, or an idle channel - and "no note" is the true answer.
			if (finePitch != nullptr)
			{
				*finePitch = 0;
			}

			return -1;
		}
	};

	Table table;
	Voice voices[kChannels];

	/** Patterns keyed by the four things that decide one. A driver rewrites taps constantly, so
		this is not an optimisation of a rare case: without it every frame would walk up to 4096
		states per channel. */
	mutable std::map<uint64_t, std::shared_ptr<const std::vector<float>>> patterns;

	std::shared_ptr<const std::vector<float>> patternFor (const Voice& voice) const
	{
		const uint64_t key = ((uint64_t) voice.shiftReg << 32) | ((uint64_t) voice.taps << 16)
							 | ((uint64_t) (voice.volume & 0xFF) << 8)
							 | (voice.integrate ? 1ull : 0ull);

		const auto cached = patterns.find (key);

		if (cached != patterns.end())
		{
			return cached->second;
		}

		auto points = std::make_shared<std::vector<float>> ();

		// **No cycle, no wave - and `nullptr`, not an empty vector.** An empty wave draws as
		// "0 pts", which reads like a decoder bug; `null` says the honest thing, that this channel
		// is producing something with no drawable cycle (noise here, and the row falls back to the
		// "no shape" state the other noise channels use).
		if (! mikey::buildPattern (voice.shiftReg, voice.taps, voice.volume, voice.integrate,
								   *points))
		{
			patterns.emplace (key, nullptr);
			return nullptr;
		}

		patterns.emplace (key, points);
		return points;
	}
};

MikeyDecoder::MikeyDecoder() = default;
MikeyDecoder::~MikeyDecoder() = default;

void MikeyDecoder::reset (const ChipConfig& config)
{
	const uint32_t clock = (config.clockHz != 0) ? config.clockHz : kDefaultClock;

	impl = std::make_unique<Impl>();
	impl->table.clock = clock;
}

void MikeyDecoder::write (const RegisterWrite& write)
{
	if (impl == nullptr)
	{
		return;
	}

	// The four audio channels live at 0x20..0x3F, eight registers each; 0x40 and above are the
	// stereo attenuators, which shape the mix but carry no pitch.
	if (write.reg < 0x20 || write.reg >= 0x40)
	{
		return;
	}

	const int channel = (write.reg >> 3) & 0x03;

	Impl::Voice& voice = impl->voices[channel];
	const int value = (int) write.value;

	switch (write.reg & 0x07)
	{
	case 0x0:		// VOLCNTRL - a signed level, nothing else (`mikey.c:414`)
		voice.volume = (std::int8_t) (uint8_t) value;
		break;

	case 0x1:		// FEEDBACK - the tap selector, six bits here and two shifted up (`mikey.c:420`)
		voice.taps = (uint16_t) ((voice.taps & 0x3C0) | (value & 0x3F)
								 | ((value & 0xC0) << 4));
		break;

	case 0x3:		// SHIFT - the register's low eight bits (`mikey.c:430`)
		voice.shiftReg = (uint16_t) ((voice.shiftReg & 0x0F00) | (value & 0xFF));
		break;

	case 0x4:		// BACKUP - the reload value the counter borrows back to
		voice.backup = value & 0xFF;
		break;

	case 0x5:		// CTRLA - clock shift in bits 0-2, integrate in bit 5, a tap in bit 7
		voice.shift = value & 0x07;
		voice.integrate = (value & 0x20) != 0;
		voice.taps = (uint16_t) ((voice.taps & 0xF7F) | (value & 0x80));
		break;

	case 0x7:		// OTHER - control B's low nibble, the register's top four bits above it
		voice.shiftReg = (uint16_t) ((voice.shiftReg & 0x0FF) | ((value & 0xF0) << 4));
		break;

	default:
		break;		// 0x2 OUTPUT (a PCM channel's write, unused here), 0x6 the live counter
	}
}

void MikeyDecoder::advance (uint32_t)
{
}

ChannelState MikeyDecoder::channelState (int channel) const
{
	ChannelState state;
	if (impl == nullptr || channel < 0 || channel >= kChannels)
	{
		return state;
	}

	const Impl::Voice& voice = impl->voices[channel];
	state.present = true;
	state.gate = voice.volume != 0 && voice.backup > 0;
	state.volume = std::min (15, std::abs (voice.volume));
	state.period = (uint32_t) ((voice.shift << 8) | (voice.backup & 0xFF));

	if (! state.gate)
	{
		return state;
	}

	const double stepRate = stepRateOf (impl->table.clock, voice.backup, voice.shift);


	state.wave = impl->patternFor (voice);

	if (state.wave != nullptr && state.wave->size () >= 2)
	{
		// **The fundamental depends on the pattern's length.** A two-step cycle is the classic
		// square at half the step rate; a seven-step one is a seventh of it. Reporting the step rate
		// as the pitch would make every noise channel look like a high tone.
		const double steps = (double) state.wave->size ();
		state.hz = stepRate / steps;
		state.sampleRate = (float) stepRate;
		state.tableWave = true;
	}
	else
	{
		// No cycle short enough to call a waveform: the step rate is still the only number the
		// registers state, and `unpitched` says not to pretend it is a note.
		state.hz = 0.0;
		state.pitchSource = PitchSource::unpitched;
		return state;
	}

	state.pitchSource = PitchSource::registerExact;
	return state;
}

const NoteTable& MikeyDecoder::noteTable (int) const
{
	static const Impl::Table fallback;
	return impl != nullptr ? impl->table : fallback;
}

ChannelKind MikeyDecoder::channelKind (int channel) const
{
	if (impl == nullptr || channel < 0 || channel >= kChannels)
	{
		return ChannelKind::sn76489Square;
	}

	// **`wavetableWave`, not `sn76489Square`.** The pattern is not a fifty-percent pulse: a
	// four-step cycle can be two highs and two lows, one and three, or three and one, and that duty
	// is the shape the chip produces. A pulse kind would make the display and the export synthesise
	// a square from `duty` and throw the pattern away.
	const auto wave = impl->patternFor (impl->voices[channel]);

	return (wave == nullptr || wave->size () < 2) ? ChannelKind::ayNoise
												  : ChannelKind::wavetableWave;
}

} // namespace toybox::chips
