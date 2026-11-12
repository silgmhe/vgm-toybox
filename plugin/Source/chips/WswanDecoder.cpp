#include "WswanDecoder.h"

#include <algorithm>
#include <array>
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
	constexpr uint32_t kDefaultClock = 3072000;	// the WonderSwan's 3.072 MHz crystal

	// The period field is 11 bits wide, so the two cases without a divider - the noise generator
	// and the voice stream - are flagged in the bits above it, as on the HuC6280.
	constexpr uint32_t kNoiseFlag = 0x1000;
	constexpr uint32_t kVoiceFlag = 0x2000;

	constexpr int kVoiceNote = 60;					// trigger marker, not the sample's pitch
	constexpr uint32_t kVoiceLength = 4410;	// 100 ms at 44.1 kHz

	/** Register 0x90's enable byte: the noise channel in bit 7, the sweep unit in bit 6 and the
		voice DMA in bit 5, with the four tone channels in the low bits, which this decoder reads
		as a group rather than one at a time. */
	constexpr uint8_t kModeNoiseEnable = 0x80;
	constexpr uint8_t kModeSweepEnable = 0x40;
	constexpr uint8_t kModeVoiceEnable = 0x20;

	/** A channel's frequency register is 11 bits wide. The flags above are the top bits of the
		same 16-bit word, which is why 0x1000 and 0x2000 are period flags rather than a second
		register - and why a period read has to mask them off first. */
	constexpr uint32_t kFrequencyMask = 0x07FF;

	/** **The waveform tables are not registers.** A channel's shape is thirty-two 4-bit samples at
		`ram[(waveBase + channel * 0x10) & 0xFFF0]`, and the driver uploads them one byte at a time
		through the same command stream (`0xC6`), not through a register file. The addresses below
		come from `ws_audio.c:339`: two samples per byte, the **low** nibble first. */
	constexpr int kWaveSteps = 32;
	constexpr int kWaveBytes = kWaveSteps / 2;
	constexpr int kChannelWaveStride = 0x10;

	/** The sound hardware only sees the first 16 KB of the console's RAM (`ws_audio.c:177`). */
	constexpr std::size_t kRamBytes = 0x4000;
	constexpr uint16_t kWaveOffsetMask = 0xFFF0;

	/** `0x8F` stores the base in units of 64 bytes (`ws_audio.c:439`). */
	constexpr int kWaveBaseShift = 6;

	/** A 4-bit sample is centred on 8, and the core turns it into -0x80..+0x70 before mixing
		(`ws_audio.c:345`), so a decoded step is `(value - 8) / 8`. */
	constexpr float kWaveCentre = 8.0f;

	/** **A cache with a bound, not a container that grows.** A driver rewrites the tables as it
		changes instrument - one of these songs uses 121 distinct shapes on a single channel - so
		the decode has to be a hit, and the bound has to be larger than a song's vocabulary. Each
		entry is 32 floats, so 256 of them are 32 KB. */
	constexpr std::size_t kWaveCacheLimit = 256;

	/** FNV-1a over one table's sixteen bytes. */
	uint64_t hashOfTable (const uint8_t* bytes)
	{
		uint64_t hash = 1469598103934665603ull;

		for (int i = 0; i < kWaveBytes; ++i)
		{
			hash = (hash ^ (uint64_t) bytes[i]) * 1099511628211ull;
		}

		return hash;
	}

	/** Wave: the core steps the 32-entry waveform once every 32 * (2048 - frequency) clocks, so
		one cycle takes 32 steps and frequency 0 is the lowest pitch rather than the highest.

		Noise: the LFSR is clocked at that same step rate, which gives the generator no pitch of
		its own - the rate picks a GM drum key instead. The divider spans exactly eleven octaves
		(2048 down to 1), more than the drum key range, so it is compressed onto twenty keys. */
	class WswanTable final : public NoteTable
	{
	public:
		explicit WswanTable (double clock) : clock (clock) {}

		int noteFor (uint32_t period, int, int* finePitch) const override
		{
			if ((period & kVoiceFlag) != 0)
			{
				if (finePitch != nullptr)
				{
					*finePitch = 0;
				}

				return kVoiceNote;
			}

			if ((period & kNoiseFlag) != 0)
			{
				return noiseNote (period & kFrequencyMask, finePitch);
			}

			return quantiseFrequency (toneHz (period & kFrequencyMask), finePitch);
		}

	private:
		double toneHz (uint32_t frequency) const
		{
			const double divider = 2048.0 - (double) frequency;
			return clock / (32.0 * std::max (divider, 1.0));
		}

		int noiseNote (uint32_t frequency, int* finePitch) const
		{
			if (finePitch != nullptr)
			{
				*finePitch = 0;
			}

			const double divider = 2048.0 - (double) frequency;
			const double position = std::log2 (2048.0 / std::max (divider, 1.0)) / 11.0;
			const long level = std::lround (std::clamp (position, 0.0, 1.0) * 20.0);

			return kDrumBaseKey + (int) level;
		}

		static constexpr int kDrumBaseKey = 38;		// GM acoustic snare, where the 2A03 starts

		double clock;
	};
}

struct WswanDecoder::Impl
{
	struct Channel
	{
		uint16_t frequency = kFrequencyMask;	// the chip powers up at 0x07FF, which is not silence
		uint8_t left = 0;
		uint8_t right = 0;
	};

	Channel channels[4];
	uint8_t mode = 0;				// 0x90: per-channel enable, voice, sweep, noise
	uint8_t pcmVolume = 0;			// 0x94
	int8_t sweepStep = 0;			// 0x8C, signed: it counts up or down
	uint32_t sweepInterval = 1;	// 0x8D, in samples
	uint16_t sweepPeriod = 0;		// where the sweep unit currently is
	uint32_t sweepNext = 0;
	uint32_t voiceEnd = 0;
	uint32_t now = 0;
	double clock = (double) kDefaultClock;
	double sampleRate = 44100.0;
	std::unique_ptr<WswanTable> table;

	/** **The 16 KB of RAM the sound hardware can see**, uploaded a byte at a time by `0xC6`.

		This is the chip's whole waveform memory, not a per-channel register: a channel points into
		it with `0x8F` and the four tables sit sixteen bytes apart. Keeping the image rather than
		just the four current tables means a driver that moves the base, or rewrites one table while
		another plays, needs no special case - the snapshot reads wherever the pointer is now.
	*/
	std::array<uint8_t, kRamBytes> ram = {};
	uint16_t waveBase = 0;			// 0x8F << 6

	/** Decoded tables keyed by their sixteen bytes. `mutable` because `channelState` is const and
		the vector it hands out is immutable once decoded. */
	mutable std::map<uint64_t, std::shared_ptr<const std::vector<float>>> waveCache;

	bool sweepRunning (void) const { return sweepStep != 0 && (mode & kModeSweepEnable) != 0; }
	bool voiceRunning (void) const { return (mode & kModeVoiceEnable) != 0; }
	bool noiseRunning (void) const { return (mode & kModeNoiseEnable) != 0; }

	/** **The table one channel walks**, thirty-two steps of four bits read out of `ram`.

		The nibble order is the core's, and it is not the obvious one: an **even** step takes the
		**low** nibble of its byte and an odd step the high one (`ws_audio.c:339-344`). Getting this
		backwards would mirror every two-step pair, which reads as a slightly different waveform
		rather than as an error - the reason the tests pin both steps of the first byte.
	*/
	std::shared_ptr<const std::vector<float>> waveFor (int channel) const
	{
		const std::size_t base = (std::size_t) ((waveBase + channel * kChannelWaveStride)
												 & kWaveOffsetMask);
		const uint64_t key = hashOfTable (ram.data () + base);

		const auto found = waveCache.find (key);

		if (found != waveCache.end ())
		{
			return found->second;
		}

		auto wave = std::make_shared<std::vector<float>> ();
		wave->reserve (kWaveSteps);

		for (int step = 0; step < kWaveSteps; ++step)
		{
			const uint8_t byte = ram[(base + (std::size_t) (step / 2)) & (kRamBytes - 1)];
			const uint8_t nibble = (step % 2 == 0) ? (uint8_t) (byte & 0x0F) : (uint8_t) (byte >> 4);

			wave->push_back (((float) nibble - kWaveCentre) / kWaveCentre);
		}

		if (waveCache.size () >= kWaveCacheLimit)
		{
			// A song's vocabulary is a few dozen shapes; a driver that writes a new one every
			// frame must not grow this for the length of the song.
			waveCache.clear ();
		}

		return waveCache.emplace (key, std::move (wave)).first->second;
	}
};

WswanDecoder::WswanDecoder() : impl (std::make_unique<Impl>())
{
	reset (ChipConfig { kDefaultClock, 44100.0 });
}

WswanDecoder::~WswanDecoder() = default;

void WswanDecoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	impl = std::make_unique<Impl>();

	// a missing clock in the header is common: the WonderSwan clock is the CPU crystal
	impl->clock = (clock > 100000) ? (double) clock : (double) kDefaultClock;
	impl->sampleRate = (sampleRate > 0.0) ? sampleRate : 44100.0;
	impl->table = std::make_unique<WswanTable> (impl->clock);

	setSweepTime (0x1F, 0);		// the value the chip powers up with
}

void WswanDecoder::setSweepTime (uint8_t value, uint32_t sampleTime)
{
	// one tick every (n + 1) sweep units, a unit being 32 HBlanks of 256 clocks each
	const double hblanks = (double) (((uint32_t) value + 1u) * 32u + 1u);
	const double samples = hblanks * 256.0 * impl->sampleRate / impl->clock;

	impl->sweepInterval = std::max<uint32_t> (1u, (uint32_t) std::lround (samples));
	impl->sweepNext = sampleTime + impl->sweepInterval;
}

void WswanDecoder::write (const RegisterWrite& w)
{
	Impl::Channel& channel0 = impl->channels[WswanDecoder::channel0];
	Impl::Channel& channel1 = impl->channels[WswanDecoder::channel1];
	Impl::Channel& channel2 = impl->channels[WswanDecoder::channel2];
	Impl::Channel& channel3 = impl->channels[WswanDecoder::channel3];

	// **A RAM byte, not a register.** The two share the command stream, and a low address would
	// look exactly like a register number, which is why the space travels as a flag rather than as
	// a convention (`0x82` is channel 1's pitch low byte and `0x0082` is a byte of RAM).
	if (w.isMemory)
	{
		const std::size_t address = (((std::size_t) w.port << 8) | (std::size_t) w.reg);


		if (address < impl->ram.size ())
		{
			impl->ram[address] = (uint8_t) w.value;
		}

		return;
	}

	switch (w.reg)
	{
	// The WonderSwan sound registers, by address - the numbers alone say nothing:
	//
	//   0x80/0x81 … 0x86/0x87  channel 0-3 frequency, low byte then high byte; channel 2 shares
	//                          its registers with the sweep unit, which is why 0x84 latches the
	//                          period as well
	//   0x88..0x8B             channel 0-3 volume (4 bit)
	//   0x89                   doubles as the PCM data port while voice mode streams, hence the
	//                          separate case below
	//   0x8C                   sweep step, signed
	//   0x8D                   sweep time
	//   0x8E                   noise LFSR tap - the noise's colour, not its pitch
	//   0x8F                   waveform base address - the shape, not the pitch
	//   0x90                   control: per-channel enable, channel 2 sweep, voice mode
	//   0x94                   PCM volume for the voice stream
	case 0x80:
		channel0.frequency = (uint16_t) ((channel0.frequency & 0xFF00) | w.value);
		break;
	case 0x81:
		channel0.frequency = (uint16_t) ((channel0.frequency & 0x00FF)
											 | ((uint16_t) w.value << 8));
		break;
	case 0x82:
		channel1.frequency = (uint16_t) ((channel1.frequency & 0xFF00) | w.value);
		break;
	case 0x83:
		channel1.frequency = (uint16_t) ((channel1.frequency & 0x00FF)
											 | ((uint16_t) w.value << 8));
		break;
	case 0x84:
		channel2.frequency = (uint16_t) ((channel2.frequency & 0xFF00) | w.value);
		impl->sweepPeriod = channel2.frequency;
		break;
	case 0x85:
		channel2.frequency = (uint16_t) ((channel2.frequency & 0x00FF)
											 | ((uint16_t) w.value << 8));
		impl->sweepPeriod = channel2.frequency;
		break;
	case 0x86:
		channel3.frequency = (uint16_t) ((channel3.frequency & 0xFF00) | w.value);
		break;
	case 0x87:
		channel3.frequency = (uint16_t) ((channel3.frequency & 0x00FF)
											 | ((uint16_t) w.value << 8));
		break;
	case 0x88:
	case 0x8A:
	case 0x8B:
	{
		Impl::Channel& current = impl->channels[w.reg - 0x88];
		current.left = (uint8_t) ((w.value >> 4) & 0x0F);
		current.right = (uint8_t) (w.value & 0x0F);
		break;
	}
	case 0x89:
		// the PCM sample byte, not a volume: it only matters while voice mode streams it
		if (impl->voiceRunning() && w.sampleTime >= impl->voiceEnd)
		{
			// a stream of DMA bytes: rate-limit the trigger so the channel reads as drum hits
			// instead of one note per byte
			impl->voiceEnd = w.sampleTime + kVoiceLength;
		}
		break;
	case 0x8C:
		impl->sweepStep = (int8_t) w.value;
		break;
	case 0x8D:
		setSweepTime (w.value, w.sampleTime);
		break;
	case 0x8E:
		break;		// LFSR tap: a different noise colour, the same rate
	case 0x8F:
		// **Where the four tables are.** The register counts 64-byte units and the channels sit
		// sixteen bytes apart from there (`ws_audio.c:439-442`); it changes no note, only which
		// bytes the row draws and the chip walks.
		impl->waveBase = (uint16_t) (w.value << kWaveBaseShift);
		break;
	case 0x90:
	{
		const uint8_t previous = impl->mode;
		impl->mode = w.value;

		if ((previous & 0x20) == 0 && (w.value & 0x20) != 0)
		{
			impl->voiceEnd = w.sampleTime + kVoiceLength;
		}

		if ((previous & 0x40) == 0 && (w.value & 0x40) != 0)
		{
			impl->sweepNext = w.sampleTime + impl->sweepInterval;
		}
		break;
	}
	case 0x94:
		impl->pcmVolume = w.value;
		break;
	default:
		break;		// 0x91-0x93: master volume and the LFSR read-back, nothing to hear
	}
}

void WswanDecoder::stepSweep (uint32_t sampleTime)
{
	if (! impl->sweepRunning() || sampleTime < impl->sweepNext)
	{
		return;
	}

	// every step that falls into the elapsed span, applied at once: between two polls only the
	// resulting pitch can be observed
	const uint32_t steps = (sampleTime - impl->sweepNext) / impl->sweepInterval + 1u;
	const int value = (int) impl->sweepPeriod + (int) impl->sweepStep * (int) steps;

	impl->sweepPeriod = (uint16_t) (value & 0x07FF);
	impl->sweepNext += steps * impl->sweepInterval;
}

void WswanDecoder::advance (uint32_t sampleTime)
{
	impl->now = sampleTime;
	stepSweep (sampleTime);
}

ChannelState WswanDecoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= channelCount_)
	{
		return state;
	}

	const Impl::Channel& current = impl->channels[channel];
	const int volume = std::max<int> (current.left, current.right);
	const bool enabled = (impl->mode & (1 << channel)) != 0;

	state.present = true;
	state.patch = 0;
	state.volume = volume;

	if (channel == WswanDecoder::channel1 && impl->voiceRunning())
	{
		// the core plays the DMA byte through the PCM volume, not the waveform: no pitch, and
		// the hit is over when the buffer's worth of bytes has been sent
		const int pcmVolume = (impl->pcmVolume & 0x0C) * 2;		// the core's 0..24
		state.gate = impl->now < impl->voiceEnd && pcmVolume > 0;
		state.period = kVoiceFlag;
		state.volume = pcmVolume * 15 / 24;
		return state;
	}

	if (channel == WswanDecoder::channel3 && impl->noiseRunning())
	{
		// 0xFFFF stops the LFSR on channel 3 the same way it stops the waveform on the others
		state.gate = enabled && volume > 0 && current.frequency != 0xFFFF;
		state.period = kNoiseFlag | (current.frequency & 0x07FF);
		return state;
	}

	// 0xFFFF is the one value the core treats as silence on a tone channel
	const bool silent = current.frequency == 0xFFFF && ! (channel == WswanDecoder::channel2
														  && impl->sweepRunning());
	const uint32_t period = (channel == WswanDecoder::channel2 && impl->sweepRunning())
									 ? (uint32_t) (impl->sweepPeriod & 0x07FF)
									 : (uint32_t) (current.frequency & 0x07FF);

	state.gate = enabled && volume > 0 && ! silent;
	state.period = period;

	// **The row draws the table the channel walks**, one cycle by definition, at the rate the
	// counter steps it: `clock / (2048 - f)` steps a second, whose thirty-second is the
	// `clock / (32 * (2048 - f))` the note table already reports. Drawing a table even while the
	// channel is silent is deliberate - a driver loads a shape before it keys on, and a row with
	// nothing to drag is worse than a flat one.
	state.wave = impl->waveFor (channel);
	state.tableWave = true;
	state.sampleRate = (float) (impl->clock / (double) (2048 - (period & kFrequencyMask)));

	return state;
}

const NoteTable& WswanDecoder::noteTable (int channel) const
{
	(void) channel;		// one table: the tone, noise and voice cases differ only in the period
	return *impl->table;
}

ChannelKind WswanDecoder::channelKind (int channel) const
{
	if (channel == WswanDecoder::channel1 && impl->voiceRunning())
	{
		return ChannelKind::adpcmDrum;
	}

	if (channel == WswanDecoder::channel3 && impl->noiseRunning())
	{
		return ChannelKind::wsNoise;
	}

	return ChannelKind::wsWave;
}

bool WswanDecoder::isDrum (int channel) const
{
	return channel == WswanDecoder::channel3 && impl->noiseRunning();
}

} // namespace toybox::chips
