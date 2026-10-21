#include "K054539Decoder.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <vector>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	// Konami's boards clock the K054539 at 18.432 MHz; the header supplies the real one.
	constexpr std::uint32_t kDefaultClock = 18432000;

	// k054539.c: the device renders at `clock / 384` (48 kHz at the default clock).
	constexpr double kOutputDivisor = 384.0;

	// One output sample adds the 24-bit pitch to a 16-bit fraction and fetches a unit whenever it
	// carries, so the unit rate is `pitch * clock / (65536 * 384)`. A unit is a byte for 8-bit PCM,
	// a word for 16-bit PCM and a nibble for 4-bit DPCM; the rate below is in those units.
	constexpr double kStepDivisor = 65536.0 * 384.0;

	constexpr std::uint16_t kKeyOnRegister = 0x214;
	constexpr std::uint16_t kKeyOffRegister = 0x215;
	constexpr std::uint16_t kControlRegister = 0x22F;
	constexpr std::uint8_t kControlPcmEnable = 0x01;

	// The formats the type register selects (`base2[0] & 0xc` in the core).
	constexpr std::uint8_t kTypeEightBit = 0x00;
	constexpr std::uint8_t kTypeSixteenBit = 0x04;
	constexpr std::uint8_t kTypeFourBit = 0x08;

	// The unit that ends a sample, one per format.
	constexpr std::uint8_t kEndMarkerEightBit = 0x80;
	constexpr std::uint16_t kEndMarkerSixteenBit = 0x8000;
	constexpr std::uint8_t kEndMarkerFourBit = 0x88;

	// 4-bit DPCM deltas (`dpcm` in k054539.c). Unlike the K053260's table the eighth entry is a
	// second zero rather than -128: this chip's nibbles are seven steps either way from silence.
	constexpr int kDpcmTable[16] = { 0, 1, 2, 4, 8, 16, 32, 64, 0, -64, -32, -16, -8, -4, -2, -1 };

	// Above this the "pitch" is a measurement artifact: a drum's bytes often correlate at a handful
	// of samples, and no music on these boards is written up there (C8).
	constexpr int kHighestMelodicNote = 108;

	// How far a marker scan may run before the sample is treated as unterminated, and how much of a
	// sample the measurement looks at: 8192 units is eight cycles of a 500 Hz note at 48 kHz.
	constexpr std::size_t kMaxMarkerScan = 1 << 18;
	constexpr std::size_t kMeasurementUnits = 8192;

	/** One note table per voice: the pitch register is a rate, and the sample each voice plays has
		its own cycle length. `melodic` says whether the length turned into a pitch somebody wrote;
		when it did not, the voice is a drum and the note is the C4 trigger. */
	class K054539NoteTable final : public NoteTable
	{
	public:
		K054539NoteTable (double clock, const double* sampleCycle, const bool* melodic,
						  const std::vector<std::uint8_t>* rom)
			: unitsPerSecondPerStep (clock / kStepDivisor)
			, rootRate (clock / kOutputDivisor)
			, cycleLength (sampleCycle)
			, melodic (melodic)
			, rom (rom) {}

		int noteFor (std::uint32_t period, int, int* finePitch) const override
		{
			const double rate = (double) period * unitsPerSecondPerStep;

			if (melodic != nullptr && *melodic && cycleLength != nullptr && *cycleLength > 0.0)
			{
				return quantiseFrequency (rate / *cycleLength, finePitch);
			}

			if (rom == nullptr || rom->empty())
			{
				// no ROM: the register against the chip's own rate, the top of the register being
				// one unit per output sample - middle C
				return quantisePlaybackRate (rate, rootRate, finePitch);
			}

			if (finePitch != nullptr)
			{
				*finePitch = 0;
			}

			return 60;		// a drum: the one-shot trigger, not a pitch
		}

	private:
		double unitsPerSecondPerStep;
		double rootRate;
		const double* cycleLength;
		const bool* melodic;
		const std::vector<std::uint8_t>* rom;
	};

	/** What a measurement belongs to: the registers that name one sample's bytes. */
	struct SampleKey
	{
		std::uint32_t start;
		std::uint8_t type;
		bool loop;
		std::uint32_t loopAddress;

		bool operator< (const SampleKey& other) const
		{
			return std::tie (start, type, loop, loopAddress)
				   < std::tie (other.start, other.type, other.loop, other.loopAddress);
		}
	};

	std::uint32_t pow2Mask (std::size_t size)
	{
		if (size == 0)
		{
			return 0;
		}

		std::uint32_t mask = (std::uint32_t) (size - 1);
		mask |= mask >> 1;
		mask |= mask >> 2;
		mask |= mask >> 4;
		mask |= mask >> 8;
		mask |= mask >> 16;
		return mask;
	}
}

struct K054539Decoder::Impl
{
	struct Voice
	{
		std::uint32_t pitch = 0;		// 24 bits
		std::uint8_t volume = 0;		// attenuation: 0 is loudest, 0x40 is -36 dB
		std::uint8_t reverb = 0;
		std::uint8_t pan = 0;			// 1..0x0F right, 0x10 centre, 0x11..0x1F left
		std::uint32_t loopAddress = 0;
		std::uint32_t start = 0;		// also the live position; latched at key-on
		std::uint8_t type = kTypeEightBit;
		bool loop = false;
		bool reverse = false;

		// latched where the core latches its pointer and fraction
		bool keyed = false;
		std::uint32_t latchStart = 0;
		std::uint32_t gateEnd = 0;
		double cycleLength = 0.0;		// one cycle in units; 0 = no period (a drum)
		bool melodic = false;
	};

	Voice voices[numChannels];
	std::uint32_t now = 0;
	double clock = (double) kDefaultClock;
	double streamRate = 44100.0;
	std::vector<std::uint8_t> rom;
	std::uint32_t romMask = 0;
	std::uint8_t mode = 0;				// 0x22F: bit 0 gates all sound, as in the core
	std::map<SampleKey, double> cycleCache;
	std::map<std::pair<std::uint32_t, std::uint8_t>, std::size_t> lengthCache;
	std::vector<std::unique_ptr<K054539NoteTable>> tables;

	std::uint8_t romByte (std::uint32_t address) const
	{
		const std::uint32_t index = address & romMask;
		return (index < rom.size()) ? rom[index] : 0;
	}

	/** The bytes one step covers: two for 16-bit PCM, half a byte for 4-bit DPCM, otherwise one. */
	bool isSixteenBit (std::uint8_t type) const { return type == kTypeSixteenBit; }
	bool isFourBit (std::uint8_t type) const { return type == kTypeFourBit; }

	/** The unit a voice fetches at `index` steps past `start`, in the format's own units. 4-bit
		DPCM returns the raw nibble: it is a delta and has to be summed by the caller. */
	double fetchUnit (std::uint32_t start, std::uint8_t type, std::size_t index) const
	{
		if (isSixteenBit (type))
		{
			const std::uint32_t address = start + (std::uint32_t) (index * 2);
			const int low = romByte (address);
			const int high = romByte (address + 1);
			return (double) (std::int16_t) (low | (high << 8));
		}

		if (isFourBit (type))
		{
			// the core doubles the position for 4-bit samples, so the low nibble of a byte comes
			// first and the address advances once per two steps
			const std::uint8_t byte = romByte (start + (std::uint32_t) (index / 2));
			return (double) ((index % 2 == 0) ? (byte & 0x0F) : (byte >> 4));
		}

		return (double) (std::int8_t) romByte (start + (std::uint32_t) index);
	}

	bool isEndMarker (std::uint32_t start, std::uint8_t type, std::size_t index) const
	{
		if (isSixteenBit (type))
		{
			return fetchUnit (start, type, index) == (double) kEndMarkerSixteenBit;
		}

		if (isFourBit (type))
		{
			return romByte (start + (std::uint32_t) (index / 2)) == kEndMarkerFourBit;
		}

		return romByte (start + (std::uint32_t) index) == kEndMarkerEightBit;
	}

	/** How many units a sample holds before its end marker.

		The core keys the voice off when the unit it fetched is the marker, so the distance to the
		marker *is* the note's length - exact, and the reason this chip needs no gate guess. A
		sample with no marker in reach is treated as unterminated (the song is malformed, or the
		marker is past the ROM); the caller then uses a two-second gate instead of a number that
		the scan invented. */
	std::size_t sampleUnits (std::uint32_t start, std::uint8_t type)
	{
		const auto key = std::make_pair (start, type);
		const auto cached = lengthCache.find (key);
		if (cached != lengthCache.end())
		{
			return cached->second;
		}

		std::size_t units = kMaxMarkerScan;
		for (std::size_t i = 0; i < kMaxMarkerScan; ++i)
		{
			if (isEndMarker (start, type, i))
			{
				units = i;
				break;
			}
		}

		lengthCache[key] = units;
		return units;
	}

	/** A byte address in the sample turned into a step index (the loop register is a byte
		address; the position is in the format's units). */
	std::size_t stepsForAddress (std::uint32_t address, std::uint32_t start, std::uint8_t type) const
	{
		const std::uint32_t delta = (address > start) ? (address - start) : 0;
		if (isSixteenBit (type))
		{
			return (std::size_t) (delta / 2);
		}

		if (isFourBit (type))
		{
			return (std::size_t) delta * 2;
		}

		return (std::size_t) delta;
	}

	double unitsPerSecond (const Voice& voice) const
	{
		return (double) voice.pitch * clock / kStepDivisor;
	}

	/** How long a non-looping voice lasts, in stream samples. A loop jumps to its loop address
		instead of stopping, so only a key-off ends it. */
	std::uint32_t durationOf (const Voice& voice)
	{
		if (voice.loop || voice.pitch == 0)
		{
			return 0;
		}

		std::size_t units = sampleUnits (voice.latchStart, voice.type);
		if (units >= kMaxMarkerScan)
		{
			units = (std::size_t) std::llround (2.0 * unitsPerSecond (voice));
		}

		const double samples = (double) units / unitsPerSecond (voice) * streamRate;
		return (std::uint32_t) std::llround (std::min (samples, 1.0e9));
	}

	/** The cycle length of the sample a voice is about to play, measured once per sample.

		The region runs from the start to the end marker, or to the measurement window if that is
		shorter; the loop part is what a sustained note repeats, and it is the part worth measuring.
		4-bit DPCM is summed before it is measured - the nibbles are deltas, and only the summed
		waveform has the period the sample was recorded with. */
	double cycleFor (const Voice& voice)
	{
		if (rom.empty() || voice.pitch == 0)
		{
			return 0.0;
		}

		const SampleKey key { voice.latchStart, voice.type, voice.loop, voice.loopAddress };
		const auto cached = cycleCache.find (key);
		if (cached != cycleCache.end())
		{
			return cached->second;
		}

		const std::size_t total = sampleUnits (voice.latchStart, voice.type);
		const std::size_t window = std::min (total, kMeasurementUnits);
		std::vector<double> waveform (window);

		if (isFourBit (voice.type))
		{
			double accumulator = 0.0;
			for (std::size_t i = 0; i < window; ++i)
			{
				accumulator += (double) kDpcmTable[(int) fetchUnit (voice.latchStart, voice.type, i)];
				accumulator = std::clamp (accumulator, -32768.0, 32767.0);
				waveform[i] = accumulator;
			}
		}
		else
		{
			for (std::size_t i = 0; i < window; ++i)
			{
				waveform[i] = fetchUnit (voice.latchStart, voice.type, i);
			}
		}

		std::size_t loopOffset = 0;
		std::size_t loopLength = 0;
		if (voice.loop && voice.loopAddress > voice.latchStart)
		{
			const std::size_t offset = stepsForAddress (voice.loopAddress, voice.latchStart, voice.type);
			if (offset > 0 && offset < window)
			{
				loopOffset = offset;
				loopLength = window - offset;
			}
		}

		const double cycle = estimateCycleLength (waveform, loopOffset, loopLength);
		cycleCache[key] = cycle;
		return cycle;
	}

	/** The key-on edge of one channel: the core copies the position register and starts stepping
		from there, so the length and the pitch both come from the registers as they are now. */
	void keyOn (int channel, std::uint32_t sampleTime)
	{
		Voice& voice = voices[channel];
		voice.keyed = true;
		voice.latchStart = voice.start;
		voice.gateEnd = sampleTime + durationOf (voice);
		voice.cycleLength = cycleFor (voice);

		voice.melodic = false;
		if (! rom.empty() && voice.cycleLength > 0.0)
		{
			int fine = 0;
			voice.melodic =
				quantiseFrequency (unitsPerSecond (voice) / voice.cycleLength, &fine)
				<= kHighestMelodicNote;
		}
	}

	/** The volume register is an attenuation: 0 is full scale and 0x40 is -36 dB, so the level is
		ten to the minus a fraction of that. Mapped onto the extractor's 0..15. */
	int volumeOf (const Voice& voice) const
	{
		const double decibels = (double) voice.volume * 36.0 / 64.0;
		const double amplitude = std::pow (10.0, -decibels / 20.0);
		return std::clamp ((int) std::lround (amplitude * 15.0), 0, 15);
	}

	bool isSounding (int channel) const
	{
		const Voice& voice = voices[channel];

		if (! voice.keyed || (mode & kControlPcmEnable) == 0)
		{
			return false;
		}

		if (voice.pitch == 0 || volumeOf (voice) == 0)
		{
			return false;
		}

		return voice.loop || now < voice.gateEnd;
	}
};

K054539Decoder::K054539Decoder() : impl (std::make_unique<Impl>())
{
	reset (kDefaultClock, 44100.0);
}

K054539Decoder::~K054539Decoder() = default;

void K054539Decoder::reset (std::uint32_t clock, double sampleRate)
{
	// the ROM belongs to the song, not to the clock
	std::vector<std::uint8_t> rom = impl != nullptr ? std::move (impl->rom) : std::vector<std::uint8_t>();
	const std::uint32_t romMask = impl != nullptr ? impl->romMask : 0u;

	impl = std::make_unique<Impl>();
	impl->clock = (clock > 100000) ? (double) clock : (double) kDefaultClock;
	impl->streamRate = (sampleRate > 0.0) ? sampleRate : 44100.0;
	impl->rom = std::move (rom);
	impl->romMask = romMask;
	impl->tables.reserve ((std::size_t) numChannels);
	for (auto& voice : impl->voices)
	{
		impl->tables.push_back (std::make_unique<K054539NoteTable> (impl->clock, &voice.cycleLength,
																   &voice.melodic, &impl->rom));
	}
}

void K054539Decoder::setRom (std::vector<std::uint8_t> rom)
{
	impl->romMask = pow2Mask (rom.size());
	impl->rom = std::move (rom);
	impl->cycleCache.clear();
	impl->lengthCache.clear();
}

void K054539Decoder::write (const RegisterWrite& w)
{
	// the chip is addressed with sixteen bits (0xD3 carries both), so the high byte travels in the
	// write's port like the C140's
	const std::uint16_t offset = (std::uint16_t) (((std::uint16_t) w.port << 8) | w.reg);
	const std::uint8_t value = w.value;

	if (offset < 0x100)
	{
		const int channel = (int) (offset / 0x20);
		Impl::Voice& voice = impl->voices[channel];

		switch (offset & 0x1F)
		{
		case 0x00:		// pitch, low byte
			voice.pitch = (voice.pitch & 0xFFFF00u) | value;
			break;
		case 0x01:		// pitch, middle byte
			voice.pitch = (voice.pitch & 0xFF00FFu) | ((std::uint32_t) value << 8);
			break;
		case 0x02:		// pitch, high byte
			voice.pitch = (voice.pitch & 0x00FFFFu) | ((std::uint32_t) value << 16);
			break;
		case 0x03:
			voice.volume = value;
			break;
		case 0x04:
			voice.reverb = value;
			break;
		case 0x05:
			voice.pan = value;
			break;
		case 0x08:		// loop address, low byte
			voice.loopAddress = (voice.loopAddress & 0xFFFF00u) | value;
			break;
		case 0x09:
			voice.loopAddress = (voice.loopAddress & 0xFF00FFu) | ((std::uint32_t) value << 8);
			break;
		case 0x0A:
			voice.loopAddress = (voice.loopAddress & 0x00FFFFu) | ((std::uint32_t) value << 16);
			break;
		case 0x0C:		// start, low byte - and the chip's live position
			voice.start = (voice.start & 0xFFFF00u) | value;
			break;
		case 0x0D:
			voice.start = (voice.start & 0xFF00FFu) | ((std::uint32_t) value << 8);
			break;
		case 0x0E:
			voice.start = (voice.start & 0x00FFFFu) | ((std::uint32_t) value << 16);
			break;
		default:
			break;
		}
		return;
	}

	if (offset >= 0x200 && offset < 0x210)
	{
		const int channel = (int) ((offset - 0x200) / 2);
		Impl::Voice& voice = impl->voices[channel];

		if ((offset & 1) == 0)
		{
			// type byte: bits 2-3 are the format, bit 5 reverses the sample
			voice.type = (std::uint8_t) (value & 0x0C);
			voice.reverse = (value & 0x20) != 0;
		}
		else
		{
			voice.loop = (value & 0x01) != 0;
		}
		return;
	}

	switch (offset)
	{
	case kKeyOnRegister:
		for (int channel = 0; channel < channelCount_; ++channel)
		{
			if ((value & (1 << channel)) != 0)
			{
				impl->keyOn (channel, w.sampleTime);
			}
		}
		break;
	case kKeyOffRegister:
		for (int channel = 0; channel < channelCount_; ++channel)
		{
			if ((value & (1 << channel)) != 0)
			{
				impl->voices[channel].keyed = false;
				impl->voices[channel].gateEnd = 0;
			}
		}
		break;
	case kControlRegister:
		impl->mode = value;
		break;
	default:
		break;		// the timer, the ROM port and the reverb registers carry no note
	}
}

void K054539Decoder::advance (std::uint32_t sampleTime)
{
	impl->now = sampleTime;

	for (auto& voice : impl->voices)
	{
		// the core keys a non-looping voice off when its marker arrives
		if (voice.keyed && ! voice.loop && sampleTime >= voice.gateEnd)
		{
			voice.keyed = false;
		}
	}
}

ChannelState K054539Decoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= channelCount_)
	{
		return state;
	}

	const Impl::Voice& voice = impl->voices[channel];
	state.present = true;
	state.gate = impl->isSounding (channel);
	state.period = voice.pitch;
	state.volume = impl->volumeOf (voice);

	return state;
}

const NoteTable& K054539Decoder::noteTable (int channel) const
{
	const std::size_t index = (channel >= 0 && channel < channelCount_) ? (std::size_t) channel : 0;
	return *impl->tables[index];
}

Family K054539Decoder::family (int channel) const
{
	return isDrum (channel) ? Family::adpcmDrum : Family::k054539Pcm;
}

bool K054539Decoder::isDrum (int channel) const
{
	if (channel < 0 || channel >= channelCount_ || impl->rom.empty())
	{
		return false;
	}

	return ! impl->voices[channel].melodic;
}

} // namespace toybox::chips
