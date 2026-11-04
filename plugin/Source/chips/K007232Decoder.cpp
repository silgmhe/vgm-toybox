#include "K007232Decoder.h"

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	/** The core runs at clock / 128 (`K007232_CLOCKDIV`, k007232.c:50). */
	constexpr double kClockDivisor = 128.0;

	/** The counter walks thirty-two per output sample and reloads with `0x1000 - step`. */
	constexpr double kCounterWalk = 32.0;
	constexpr double kCounterRange = 4096.0;

	constexpr uint32_t kDefaultClock = 8000000;

	/** How far into a sample the period search looks, and how coarsely: the OKI's numbers. Enough
		to see a repeat in any real sample, small enough to stay off the audio thread's budget. */
	constexpr uint32_t kScanBytes = 4096;
	constexpr std::size_t kScanStep = 256;
	constexpr std::size_t kWindow = 512;

	double frequencyOf (uint32_t clock, uint16_t step)
	{
		// steps per sample = 32 / (4096 - step); the core's sample rate is clock / 128
		const double remaining = kCounterRange - (double) step;
		if (remaining <= 0.0)
		{
			return 0.0;		// step at the top of the range never advances: not a pitch
		}

		return (double) clock / (kClockDivisor / kCounterWalk * remaining);
	}
}

struct K007232Decoder::Impl
{
	struct Voice
	{
		uint8_t regs[6] = { 0, 0, 0, 0, 0, 0 };
		uint16_t step = 0;
		bool keyed = false;

		/** The seventeen-bit start address, latched from `regs[2..4]`. */
		uint32_t start = 0;

		/** `0x14`/`0x15`: the window this voice reads, shifted by seventeen bits. */
		uint8_t bank = 0;
	};

	/** Both voices share the formula, so one table serves them. */
	struct Table final : NoteTable
	{
		uint32_t clock = kDefaultClock;

		int noteFor (uint32_t period, int, int* finePitch) const override
		{
			const double hz = frequencyOf (clock, (uint16_t) period);
			if (hz <= 0.0 || hz > 4186.0)		// past MIDI's ceiling: a drum, not a dropped channel
			{
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
	Voice voices[2];
	uint8_t loop = 0;

	/** The ROM the voices read, copied by `setRom` because the assembler's image is per flush. */
	std::vector<uint8_t> rom;

	/** Decoded samples, keyed by bank and start. The ROM never changes, so the key is exact and
		the two voices share an entry when they play the same sample from the same window.
		`mutable` because `channelState` is const and the cache is a memo of a pure function of the
		ROM - the same reason the OKI decoder keeps its own map. */
	mutable std::map<uint64_t, std::shared_ptr<const std::vector<float>>> waveCache;

	/** `k007232.c:224` - the voice reads `bank + (addr & 0x1FFFF)`, and stops when past the image. */
	uint32_t addressOf (const Voice& voice) const
	{
		return ((uint32_t) voice.bank << 17) + (voice.start & 0x1FFFFu);
	}

	/** Cycles measured, keyed by bank and start: the ROM never changes, so the key is exact. */
	mutable std::map<uint64_t, double> cycleCache;

	/** **The sample's period in bytes** - what the pitch is, and what `step` on its own is not.
	 *
	 *	`frequencyOf` answers bytes per second, and this chip's rows were reporting that as a
	 *	frequency: 9943 to 29829 Hz on the test songs, i.e. MIDI 123 to 135, three octaves above the
	 *	keyboard's top note of 83. Nothing was wrong with the keyboard - the row was not describing a
	 *	pitch at all. A sampled chip knows its playback rate and has to *measure* the wave inside it,
	 *	which is what the OKI, the ESP5503 and the RF5C all do.
	 *
	 *	A drum has no period; that is 0 and reads as no note, which is the right answer for one.
	 */
	double cycleFor (const Voice& voice) const
	{
		const uint64_t key = ((uint64_t) voice.bank << 17) | (uint64_t) voice.start;
		const auto cached = cycleCache.find (key);

		if (cached != cycleCache.end())
		{
			return cached->second;
		}

		std::vector<double> values;
		values.reserve (kScanBytes);

		const uint32_t base = addressOf (voice);

		for (uint32_t i = 0; i < kScanBytes; ++i)
		{
			const uint32_t at = base + i;

			if (at >= rom.size() || (rom[at] & 0x80u) != 0)
			{
				break;
			}

			values.push_back ((double) ((int) (rom[at] & 0x7Fu) - 0x40));
		}

		double cycle = 0.0;

		// The same scan the OKI uses: slide a window along the sample, because a drum's opening
		// transient does not repeat but its body may.
		for (std::size_t offset = 0; cycle <= 0.0 && offset + 64 <= values.size();
			 offset += kScanStep)
		{
			const std::size_t length = std::min<std::size_t> (kWindow, values.size() - offset);
			const std::vector<double> window (values.begin() + (long) offset,
											  values.begin() + (long) (offset + length));
			cycle = bestPeriod (window, kWindow / 2);
		}

		cycleCache.emplace (key, cycle);
		return cycle;
	}

	/** The bytes a voice is playing, decoded to -1..1.
	 *
	 *	Seven-bit unsigned with `0x40` as silence (`k007232.c:232`), and the sample ends at the first
	 *	byte with **bit 7 set** (`k007232.c:246`) - the length is in the data, not in a register.
	 */
	std::shared_ptr<const std::vector<float>> decodeVoice (const Voice& voice) const
	{
		const uint64_t key = ((uint64_t) voice.bank << 32) | (uint64_t) voice.start;
		const auto found = waveCache.find (key);

		if (found != waveCache.end())
		{
			return found->second;
		}

		auto wave = std::make_shared<std::vector<float>>();

		const uint32_t base = addressOf (voice);

		// A window is 128K bytes and the terminator usually arrives long before that; the bound is
		// only there so a ROM that never sets bit 7 cannot run away.
		for (uint32_t i = 0; i < 0x20000u; ++i)
		{
			const uint32_t at = base + i;

			if (at >= rom.size())
			{
				break;
			}

			const uint8_t value = rom[at];

			if ((value & 0x80u) != 0)
			{
				break;		// the end marker, not part of the sample
			}

			wave->push_back ((float) ((int) (value & 0x7Fu) - 0x40) / 64.0f);
		}

		waveCache.emplace (key, wave);
		return wave;
	}
};

K007232Decoder::K007232Decoder() = default;
K007232Decoder::~K007232Decoder() = default;

void K007232Decoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	(void) sampleRate;
	impl = std::make_unique<Impl>();
	impl->table.clock = clock > 0 ? clock : kDefaultClock;
}

void K007232Decoder::setRom (std::vector<uint8_t> rom)
{
	impl->rom = std::move (rom);
	impl->waveCache.clear ();
}

void K007232Decoder::write (const RegisterWrite& write)
{
	if (impl == nullptr)
	{
		return;
	}

	// The board registers come after the twelve per-voice ones, and they are not decoration: the
	// bank picks which 128 KB window of the image a voice reads, and without it the second voice of
	// `02 Title Demo` would play the first voice's bytes.
	if (write.reg >= 12)
	{
		switch (write.reg)
		{
		case 0x0D:
			impl->loop = write.value;
			break;

		case 0x14:
		case 0x15:
			impl->voices[write.reg & 1].bank = write.value;
			break;

		default:
			break;		// 0x0C is the external port, 0x10..0x13 the volumes: not modelled
		}

		return;
	}

	const int channel = write.reg / 6;
	const int offset = write.reg % 6;
	Impl::Voice& voice = impl->voices[channel];
	voice.regs[offset] = write.value;

	switch (offset)
	{
	case 0x00:		// pitch, low byte
	case 0x01:		// pitch, high byte - the low nibble is the step's top bits
	{
		const uint8_t low = voice.regs[0];
		const uint8_t high = voice.regs[1];
		const uint8_t mode = (uint8_t) ((high >> 4) & 0x03);

		// only the normal twelve-bit mode is real; the core keeps the other two as placeholders and
		// falls back to the same arithmetic, so this mirrors it rather than inventing behaviour
		voice.step = (uint16_t) (((high & 0x0F) << 8) | low);
		if (mode == 0x01)
		{
			voice.step = (uint16_t) ((256 - low) << 4);
		}
		else if (mode == 0x02)
		{
			voice.step = (uint16_t) ((16 - (high & 0x0F)) << 8);
		}
		break;
	}
	case 0x02:		// start address, low
	case 0x03:		// middle
	case 0x04:		// high - only bit 0 is kept, the address is seventeen bits
	{
		// The core re-derives the address from all three bytes on any write to them
		// (k007232.c:297), so a driver writing them low to high lands on the same value.
		voice.start = ((uint32_t) (voice.regs[4] & 0x01) << 16)
					| ((uint32_t) voice.regs[3] << 8) | (uint32_t) voice.regs[2];

		// Writing the start address is what starts a voice here: not one of the six rips ever writes
		// the key register, so keying on that alone left every channel silent.
		voice.keyed = true;
		break;
	}
	case 0x05:		// key on/off: bit 0 is the key, for the drivers that do use it
		voice.keyed = (write.value & 0x01) != 0;
		break;
	default:
		break;
	}
}

void K007232Decoder::advance (uint32_t)
{
	// the writes carry every boundary; nothing to step
}

ChannelState K007232Decoder::channelState (int channel) const
{
	ChannelState state;
	if (impl == nullptr || channel < 0 || channel >= 2)
	{
		return state;
	}

	const Impl::Voice& voice = impl->voices[channel];
	state.present = true;
	state.period = voice.step;
	state.gate = voice.keyed && voice.step > 0;
	state.volume = state.gate ? 15 : 0;
	// **The pitch is the sample's period, not the rate it is read at.** `frequencyOf` answers bytes
	// per second; dividing by the measured cycle turns that into a frequency, which is what the
	// keyboard draws and what the exported file's length already implies. Reporting the rate
	// directly put every row at MIDI 123-135, above the top of the keyboard.
	const double byteRate = frequencyOf (impl->table.clock, voice.step);
	const double cycle = state.gate ? impl->cycleFor (voice) : 0.0;

	state.hz = (cycle > 0.0) ? byteRate / cycle : 0.0;
	state.pitchSource = (cycle > 0.0) ? PitchSource::sampleInferred : PitchSource::unpitched;

	// **The bytes this voice is playing.** The chip generates nothing: a row that draws it needs the
	// ROM decoded, and the sample's end lives in the data as a bit-7 marker rather than in a length
	// register.
	if (voice.keyed && ! impl->rom.empty())
	{
		state.wave = impl->decodeVoice (voice);
	}

	// **The rate, and it is per note.** This chip has no single sample rate: a voice consumes
	// `(clock / 128) * 32 / (0x1000 - step)` bytes per second, so the same bytes dragged out of two
	// notes are two different files. `frequencyOf` already computes exactly that number.
	state.sampleRate = state.gate ? (float) frequencyOf (impl->table.clock, voice.step) : 0.0f;

	return state;
}

const NoteTable& K007232Decoder::noteTable (int) const
{
	static const Impl::Table fallback;
	return impl != nullptr ? impl->table : fallback;
}

ChannelKind K007232Decoder::channelKind (int) const
{
	return ChannelKind::k053260Pcm;		// the same shape: a PCM voice with a rate register
}

} // namespace toybox::chips
