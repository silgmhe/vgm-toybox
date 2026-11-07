#include "MultiPcmDecoder.h"

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	/** Unity ratio - pitch 0, octave 1 - is placed at C4, the convention the other sampled chips
		use for a table whose absolute pitch cannot be named. */
	constexpr int kUnityNote = 60;

	constexpr int kVoices = 28;

	/** The VGM block type for this chip's samples, and the size the blocks declare. */
	constexpr uint8_t kRomType = 0x89;
	constexpr std::size_t kMaxRomBytes = 4u << 20;

	/** `multipcm.c:129` - the chip's sample rate is the master clock over 224. */
	constexpr double kClockDivisor = 224.0;
	constexpr uint32_t kDefaultClock = 8000000u;

	/** One metadata entry: start, loop, negated length, then the envelope and LFO bytes. */
	constexpr uint32_t kMetaBytes = 12;

	/** How far into a sample the cycle search looks, and over what window. The same numbers the
		other sampled decoders use: enough to see a repeat in any real sample. */
	constexpr uint32_t kScanBytes = 8 * 1024;
	constexpr std::size_t kScanWindow = 512;
	constexpr std::size_t kScanStep = 256;

	/** Above this a measured period is an artifact, not a note - the C8 ceiling the sampled players
		share. */
	constexpr int kHighestMelodicNote = 108;

	/** **Octave 1 is unity, not octave 0.** `multipcm.c:314-324` shifts by `octave - 1` and treats a
		four-bit signed field, so 0 is one octave down, 8 is seven up, and 9..15 are the octaves
		below. Reading it as `2^octave` - which this decoder did while it had no ROM - reports every
		note an octave high. */
	double octaveFactor (int octave)
	{
		const int shifted = (octave - 1) & 0x0F;

		if ((shifted & 0x8) != 0)
		{
			return std::ldexp (1.0, -(16 - shifted));
		}

		return std::ldexp (1.0, shifted);
	}

	double ratioOf (int octave, int pitch)
	{
		return ((1024.0 + (double) pitch) / 1024.0) * octaveFactor (octave);
	}
}

struct MultiPcmDecoder::Impl
{
	struct Voice
	{
		uint16_t pitch = 0;			// ten bits
		int octave = 0;						// four bits, signed
		bool keyed = false;
		int attenuation = 0x7F;
		int sampleIndex = 0;

		/** What was latched at the key-on: the metadata is read then, because a driver may change
			the index between notes and the playing voice keeps its own. */
		uint32_t base = 0;
		uint32_t length = 0;				// in samples
		uint8_t format = 0;
		double cycle = 0.0;					// measured period in samples, 0 = unmeasured
		bool melodic = false;
	};

	struct Table final : NoteTable
	{
		int noteFor (uint32_t period, int, int* finePitch) const override
		{
			const int octave = (int) ((period >> 10) & 0xFF);
			const int pitch = (int) (period & 0x3FF);
			const double ratio = ratioOf ((int8_t) (uint8_t) octave, pitch);

			if (ratio <= 0.0)
			{
				if (finePitch != nullptr)
				{
					*finePitch = 0;
				}

				return kUnityNote;
			}

			const double semitones = 12.0 * std::log2 (ratio);
			if (semitones < -48.0 || semitones > 48.0)
			{
				// outside anything a MIDI note can say; report the trigger rather than dropping it
				if (finePitch != nullptr)
				{
					*finePitch = 0;
				}

				return kUnityNote;
			}

			const double exact = (double) kUnityNote + semitones;
			const int nearest = (int) std::floor (exact + 0.5);
			if (finePitch != nullptr)
			{
				*finePitch = (int) std::lround ((exact - (double) nearest) * 100.0);
			}

			return nearest;
		}
	};

	Table table;
	Voice voices[kVoices];

	/** The chip is programmed through three ports, not by addressing a voice directly (multipcm.c:922-955):
		offset 1 selects the voice, offset 2 sets which of its eight registers the next data write
		lands in, and offset 0 writes that register. Every write in the rips goes through this door. */
	int currentSlot = -1;
	int address = 0;

	/** The sample ROM: the metadata table at 0 and the samples after it. */
	std::vector<uint8_t> rom;
	uint32_t romMask = 0;

	/** `0xC3`'s banking, arriving as writes to 0x10/0x11/0x12. */
	bool segaBanking = false;
	uint32_t bank0 = 0;
	uint32_t bank1 = 0;

	/** `multipcm.c:598` - the chip's own rate, `clock / 224`. */
	double chipRate = (double) kDefaultClock / kClockDivisor;

	mutable std::map<uint64_t, std::shared_ptr<const std::vector<float>>> waveCache;
	mutable std::map<uint64_t, double> cycleCache;

	uint8_t byteAt (uint32_t address) const
	{
		const uint32_t index = address & romMask;
		return index < rom.size() ? rom[index] : 0;
	}

	/** One sample's metadata, `multipcm.c:281-298`: a 22-bit start with the format in bit 22, and a
		length stored as its two's complement negation. */
	void readMeta (int index, uint32_t& start, uint32_t& length, uint8_t& format) const
	{
		const uint32_t at = (uint32_t) index * kMetaBytes;

		const uint32_t raw = ((uint32_t) byteAt (at) << 16) | ((uint32_t) byteAt (at + 1) << 8)
						   | (uint32_t) byteAt (at + 2);
		const uint32_t negated = ((uint32_t) byteAt (at + 5) << 8) | (uint32_t) byteAt (at + 6);

		format = (uint8_t) ((raw >> 20) & 0xFE);
		start = raw & 0x3FFFFF;
		length = 0x10000 - negated;
	}

	/** `multipcm.c:733-743` - a banked address keeps its low nineteen bits and takes its top bits
		from the bank registers. Without this the second megabyte of a 2 MB image plays the first
		megabyte's bytes. */
	uint32_t mappedBase (uint32_t start) const
	{
		if (! segaBanking)
		{
			return start;
		}

		const uint32_t window = start & 0x1FFFFF;

		if ((window & 0x100000) == 0)
		{
			return window;
		}

		return (window & 0x07FFFF) | (((window & 0x080000) != 0) ? bank1 : bank0);
	}

	/** The sample's bytes, decoded to -1..1.

		Eight-bit samples are **signed** (`multipcm.c:867`: `read_byte(...) << 8` into an `int16_t`),
		which is the one format in this project that needs no conversion at all. Twelve-bit samples
		pack two into three bytes (`multipcm.c:849-864`) and are left out: no song in
		`test_file/YMW258` uses one, and a half-understood packing would draw a waveform that is not
		the sound. The row then shows nothing for such a voice rather than something wrong.
	 */
	std::shared_ptr<const std::vector<float>> waveFor (uint32_t base, uint32_t length, uint8_t format) const
	{
		const uint64_t key = ((uint64_t) base << 20) | (uint64_t) (length & 0xFFFFF);
		const auto cached = waveCache.find (key);

		if (cached != waveCache.end())
		{
			return cached->second;
		}

		auto wave = std::make_shared<std::vector<float>>();

		if (! rom.empty() && (format & 0x04) == 0)
		{
			wave->reserve (length);

			for (uint32_t i = 0; i < length; ++i)
			{
				wave->push_back ((float) (int8_t) byteAt (base + i) / 128.0f);
			}
		}

		waveCache.emplace (key, wave);
		return wave;
	}

	/** The period inside the sample, in samples: the pitch is the playback rate divided by it. */
	double cycleFor (uint32_t base, uint32_t length) const
	{
		const uint64_t key = ((uint64_t) base << 20) | (uint64_t) (length & 0xFFFFF);
		const auto cached = cycleCache.find (key);

		if (cached != cycleCache.end())
		{
			return cached->second;
		}

		double cycle = 0.0;

		if (! rom.empty())
		{
			const uint32_t limit = (length < kScanBytes) ? length : kScanBytes;

			for (uint32_t offset = 0; cycle <= 0.0 && offset + 64 <= limit; offset += kScanStep)
			{
				const uint32_t window = ((limit - offset) < kScanWindow) ? (limit - offset)
																		: (uint32_t) kScanWindow;
				std::vector<double> waveform (window);

				for (uint32_t i = 0; i < window; ++i)
				{
					waveform[i] = (double) (int8_t) byteAt (base + offset + i);
				}

				cycle = bestPeriod (waveform, window / 2);
			}
		}

		cycleCache.emplace (key, cycle);
		return cycle;
	}

	/** `multipcm.c:671` plus the octave shift: the rate this voice reads its bytes at. */
	double playbackRate (const Voice& voice) const
	{
		return chipRate * ratioOf (voice.octave, (int) voice.pitch);
	}
};

MultiPcmDecoder::MultiPcmDecoder()
	: impl (std::make_unique<Impl>())
{
}

MultiPcmDecoder::~MultiPcmDecoder() = default;

void MultiPcmDecoder::reset (const ChipConfig& config)
{
	const uint32_t clock = (config.clockHz != 0) ? config.clockHz : kDefaultClock;
	const double sampleRate = config.timeBaseHz;

	impl = std::make_unique<Impl>();

	// The device's own rate when a device is running, the arithmetic otherwise: `multipcm.c:598`
	// starts it at `clock / 224`, and libvgm reports exactly that through `smplRate`.
	impl->chipRate = (config.deviceRateHz > 0.0) ? config.deviceRateHz
												: (double) clock / kClockDivisor;

	[[maybe_unused]] const double outRate = (sampleRate > 0.0) ? sampleRate : 44100.0;
}

void MultiPcmDecoder::setRom (std::vector<uint8_t> rom)
{
	impl->romMask = 0;
	impl->rom = std::move (rom);
	impl->waveCache.clear();
	impl->cycleCache.clear();

	if (! impl->rom.empty())
	{
		uint32_t mask = (uint32_t) (impl->rom.size() - 1);
		mask |= mask >> 1;
		mask |= mask >> 2;
		mask |= mask >> 4;
		mask |= mask >> 8;
		mask |= mask >> 16;
		impl->romMask = mask;
	}
}

void MultiPcmDecoder::write (const RegisterWrite& write)
{
	if (impl == nullptr)
	{
		return;
	}

	// Three ports, in the core's own terms: 1 selects the voice, 2 the register, 0 the data.
	if (write.reg == 0x01)
	{
		// The selector is not the voice number. The core's VALUE_TO_CHANNEL is 0..6, -1, 7..13, -1,
		// 14..20, -1, 21..27, -1 (multipcm.c:24-30): every eighth value is a spacer, so selector 8 is
		// voice 7. Reading the selector as the voice shifted every voice from 7 upwards by one, which
		// is why one channel looked audible with no notes while its neighbour took two.
		const int selector = write.value & 0x1F;
		if ((selector & 0x07) == 0x07)
		{
			impl->currentSlot = -1;
			return;
		}

		impl->currentSlot = (selector >> 3) * 7 + (selector & 0x07);
		return;
	}

	if (write.reg == 0x02)
	{
		impl->address = write.value > 7 ? 7 : write.value;
		return;
	}

	// `0x10`..`0x12` are the Sega banking registers - the loader turns VGM's `0xC3` into these
	// writes, because the command never reaches the chip (multipcm.c:940-953).
	if (write.reg == 0x10)
	{
		impl->segaBanking = true;
		impl->bank0 = ((uint32_t) write.value << 20);
		impl->bank1 = ((uint32_t) write.value << 20) | 0x080000u;
		return;
	}

	if (write.reg == 0x11)
	{
		impl->segaBanking = true;
		impl->bank0 = (uint32_t) write.value << 19;
		return;
	}

	if (write.reg == 0x12)
	{
		impl->segaBanking = true;
		impl->bank1 = (uint32_t) write.value << 19;
		return;
	}

	if (write.reg != 0x00 || impl->currentSlot < 0 || impl->currentSlot >= kVoices)
	{
		return;
	}

	const int voice = impl->currentSlot;
	Impl::Voice& state = impl->voices[voice];

	switch (impl->address)
	{
	case 0x1:		// sample index into the ROM metadata table
	{
		// The index is nine bits: the low byte, plus bit 0 of the pitch's low byte (`multipcm.c:728`).
		state.sampleIndex = (int) ((uint32_t) write.value | ((uint32_t) (state.pitch & 0x01) << 8));
		break;
	}
	case 0x2:		// pitch, low eight bits
		state.pitch = (uint16_t) ((state.pitch & 0x300) | write.value);
		state.sampleIndex = (int) ((uint32_t) (state.sampleIndex & 0xFF) | ((uint32_t) (write.value & 0x01) << 8));
		break;
	case 0x3:		// four-bit signed octave above a ten-bit pitch
		state.octave = (int8_t) (uint8_t) (write.value >> 4);
		state.pitch = (uint16_t) ((state.pitch & 0x0FF) | ((write.value & 0x03) << 8));
		break;
	case 0x4:		// bit 7 keys the voice on; clearing it releases it
	{
		const bool wasKeyed = state.keyed;
		state.keyed = (write.value & 0x80) != 0;

		if (state.keyed && ! wasKeyed)
		{
			// **Latch the sample at the key-on**, as the core does (`retrigger_sample`): the driver
			// may point the voice at another index before the next note, and this one keeps playing
			// the bytes it started on.
			uint32_t start = 0;
			uint32_t length = 0;
			uint8_t format = 0;
			impl->readMeta (state.sampleIndex, start, length, format);

			state.base = impl->mappedBase (start);
			state.length = (length > 0x10000u) ? 0x10000u : length;
			state.format = format;
			state.cycle = impl->cycleFor (state.base, state.length);

			int fine = 0;
			const double rate = impl->playbackRate (state);
			const int note = (state.cycle > 0.0) ? quantiseFrequency (rate / state.cycle, &fine) : -1;
			state.melodic = note >= 0 && note <= kHighestMelodicNote;
		}

		break;
	}
	case 0x5:		// bits 1-7 are attenuation, 0 loudest
		state.attenuation = (int) ((write.value >> 1) & 0x7F);
		break;
	default:
		break;		// 0x0 pan, 0x6/0x7 the LFOs - none of them a pitch
	}
}

void MultiPcmDecoder::advance (uint32_t)
{
}

ChannelState MultiPcmDecoder::channelState (int channel) const
{
	ChannelState state;
	if (impl == nullptr || channel < 0 || channel >= kVoices)
	{
		return state;
	}

	const Impl::Voice& voice = impl->voices[channel];
	state.present = true;
	state.gate = voice.keyed;
	state.volume = state.gate ? 15 - (voice.attenuation * 15 / 0x7F) : 0;
	state.period = (uint32_t) (((voice.octave & 0xFF) << 10) | (voice.pitch & 0x3FF));
	state.patch = voice.sampleIndex;

	if (! state.gate)
	{
		return state;
	}

	// **The pitch**, now that the ROM says where the bytes are: the rate the voice reads them at,
	// divided by the period measured inside them. `multipcm.c:671` gives the rate.
	const double rate = impl->playbackRate (voice);

	state.hz = (voice.cycle > 0.0 && voice.melodic) ? rate / voice.cycle : 0.0;
	state.pitchSource = (voice.cycle > 0.0 && voice.melodic) ? PitchSource::sampleInferred
															 : PitchSource::unpitched;

	// **The bytes and the rate a file has to declare.** The rhythm here is the chip's: one sample
	// per output frame at unity, so the declared rate is the playback rate itself.
	if (! impl->rom.empty())
	{
		state.wave = impl->waveFor (voice.base, voice.length, voice.format);
	}

	state.sampleRate = (float) rate;

	return state;
}

const NoteTable& MultiPcmDecoder::noteTable (int) const
{
	static const Impl::Table fallback;
	return impl != nullptr ? impl->table : fallback;
}

ChannelKind MultiPcmDecoder::channelKind (int) const
{
	return ChannelKind::c140Pcm;		// a sampled voice with a rate the registers scale
}

} // namespace toybox::chips
