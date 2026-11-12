#include "SegaPcmDecoder.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	constexpr int kHighestMelodicNote = 108;

	/** The reference of the transposition layer: 8000 bytes a second.

		The chip names no recording rate of its own, so this is a convention either way; 8000 is the
		figure the reference implementation divides a rate by before quantising it
		(`form/KB/PCM/frmSegaPCM.cs` -> `Common.searchSegaPCMNote`, whose argument is a multiple of
		8 kHz). It decides only which octave the transposition lands in - the sample's own measured
		root carries the absolute pitch, and `L1 + L2` does not depend on this constant. */
	constexpr double kReferenceRate = 8000.0;
	constexpr std::size_t kMeasurementUnits = 2048;

	/** How much of a sample a row draws. A slap bass is a few kilobytes - this song's is 8448 bytes,
		which the first cap of 8192 cut by exactly enough to notice - and drawing a whole drum kit's
		megabytes would cost the same and show less. */
	constexpr uint32_t kMaxWaveBytes = 16384;

	/** Sample data is unsigned bytes around 0x80, as on every chip of this generation. */
	constexpr int kSampleBias = 0x80;

	/** The control byte's bit 1 is the loop flag, and it is inverted: a cleared bit loops. */
	constexpr uint8_t kLoopFlag = 0x02;

	/** A volume register is seven bits of level; the top bit is the channel's own gate. */
	constexpr uint8_t kVolumeMask = 0x7F;

	/** The command's offset byte splits into the window (bit 7) and, inside it, eight registers
		per channel: three bits pick the register, four pick the channel. */
	constexpr uint8_t kHighWindowBit = 0x80;
	constexpr uint8_t kOffsetMask = 0x7F;
	constexpr int kChannelShift = 3;
	constexpr uint8_t kRegisterInWindowMask = 0x07;

	/** The mask the ROM is folded with: the chip addresses `bank | (addr >> 8)` into a power-of-two
		buffer, so a sample that runs off the end wraps rather than reading elsewhere. */
	uint32_t pow2Mask (std::size_t size)
	{
		if (size == 0)
		{
			return 0;
		}

		uint32_t mask = (uint32_t) (size - 1);
		mask |= mask >> 1;
		mask |= mask >> 2;
		mask |= mask >> 4;
		mask |= mask >> 8;
		mask |= mask >> 16;
		return mask;
	}

	/** One table per voice: the pitch register is a step and the sample each voice latched has its
		own cycle, so the note is `pitch * clock / 32768 / cycle`. */
	class SegaPcmNoteTable final : public NoteTable
	{
	public:
		/** `rootRate` is the rate the step register asks for at pitch 128, the midpoint every driver of
			this chip uses for a sample at its recorded pitch: the chip names no recording rate of its
			own, so the register's own midpoint is the reference the transposition is measured from. */
		SegaPcmNoteTable (double chipClock, const double* sampleCycle, const bool* sampleMelodic)
			: bytesPerSecondPerStep (chipClock > 0.0 ? chipClock / 32768.0 : 8000000.0 / 32768.0)
			, rootRate (kReferenceRate)
			, cycleLength (sampleCycle)
			, melodic (sampleMelodic) {}

		int noteFor (uint32_t pitch, int, int* finePitch) const override
		{
			const bool measured = pitch != 0 && melodic != nullptr && *melodic
						&& cycleLength != nullptr && *cycleLength > 0.0;
			return noteFromPlaybackRate ((double) pitch * bytesPerSecondPerStep, rootRate, measured, 60,
									 finePitch);
		}

	private:
		double bytesPerSecondPerStep;
		double rootRate;
		const double* cycleLength;
		const bool* melodic;
	};
}

struct SegaPcmDecoder::Impl
{
	struct Voice
	{
		// the low window of the register RAM (8n + 0..7)
		uint8_t volumeLeft = 0;
		uint8_t volumeRight = 0;
		uint8_t loopLow = 0;
		uint8_t loopHigh = 0;
		uint8_t endReg = 0;
		uint8_t pitch = 0;

		// the high window (0x80 + 8n + 0..7): current address and control
		uint8_t addressMid = 0;
		uint8_t addressHigh = 0;
		uint8_t control = 0xFF;

		bool keyed = false;
		uint32_t latchStart = 0;
		uint32_t latchEnd = 0;
		bool loops = false;
		uint32_t gateEnd = 0;
		double cycleLength = 0.0;
		bool melodic = false;
		uint32_t keyOns = 0;
	};

	Voice voices[SegaPcmDecoder::numChannels];
	std::vector<uint8_t> rom;
	uint32_t romMask = 0;
	double clock = 8000000.0;
	double streamRate = 44100.0;
	std::map<uint32_t, double> cycleCache;

	/** The bytes a voice latched, decoded, keyed by (start, length): two voices playing the same
		slap bass share one copy, and the ROM never changes under us. */
	std::map<uint64_t, std::shared_ptr<const std::vector<float>>> waveCache;
	std::unique_ptr<SegaPcmNoteTable> tables[SegaPcmDecoder::numChannels];
	uint32_t now = 0;

	uint8_t byteAt (uint32_t address) const
	{
		const uint32_t index = address & romMask;
		return index < rom.size() ? rom[index] : 0;
	}

	/** The byte rate the core plays this voice at: the position advances `pitch` in 1/256-of-a-byte
		units per sample at `clock / 128` samples a second. */
	double bytesPerSecond (const Voice& voice) const
	{
		return (double) voice.pitch * clock / 32768.0;
	}

	/** The rate pitch 128 asks for - the midpoint every driver of this chip uses for a sample at its
		recorded pitch. The chip names no recording rate of its own, so this is the reference the
		transposition layer is measured from (design doc section 14.3). */
	double referenceBytesPerSecond (void) const
	{
		return kReferenceRate;
	}

	/** **The control byte carries the ROM bank in its high bits**, and ignoring it put every voice at
		the wrong bytes: this song's writes use `control = 0x0A` and `0x12`, which are bank 8 and 16 -
		offsets 0x10000 and 0x20000 - while the address registers only say 0x0000. The row then drew
		the empty bottom of the image and the export cut silence from it, which is exactly what the
		first version looked and sounded like.

		The shift and mask come from the VGM header's SegaPCM interface bytes (0x3C / 0x3E); this file
		says 13 / 0xF8, and libvgm falls back to 0x70 when the mask is zero. Every file in this
		collection uses the same pair, so they are constants here rather than new plumbing through
		`ChipConfig` - a file that disagrees would need that plumbing, and the comment says so.
	*/
	uint8_t bankShift = 13;		// the VGM header's 0x3C - the format's usual shift
	uint8_t bankMask = 0xF8;	// ... and 0x3E, with 0x70 the fallback libvgm substitutes

	/** `offset | (addr >> 8)` exactly as the core builds it (`segapcm.c:156`, `:182`). */
	uint32_t offsetOf (const Voice& voice) const
	{
		return (uint32_t) (voice.control & bankMask) << bankShift;
	}

	/** Where the voice starts, in bytes: the 24-bit position's low byte is the chip's own fraction,
		so the address byte is `(addressHigh << 8) | addressMid`, placed in the bank the control byte
		names. */
	uint32_t startOf (const Voice& voice) const
	{
		return offsetOf (voice) | (((uint32_t) voice.addressHigh << 8) | voice.addressMid);
	}

	/** The end register counts 256-byte units and the core compares `addr >> 16`, so the sample ends
		at `(reg + 1) * 256` - inside the same bank. */
	uint32_t endOf (const Voice& voice) const
	{
		return offsetOf (voice) | ((uint32_t) (voice.endReg + 1) << 8);
	}

	double cycleFor (uint32_t start, uint32_t length)
	{
		if (rom.empty() || length == 0)
		{
			return 0.0;
		}

		const auto cached = cycleCache.find (start);
		if (cached != cycleCache.end())
		{
			return cached->second;
		}

		// The sample has no loop, so the only window is its opening - and the opening of a slap bass
		// is the slap itself: a noise burst that hides the pitch. Measure from a quarter in, where
		// the string is already ringing, and keep the first two kilobytes of that.
		const uint32_t begin = length / 4;
		const uint32_t window =
			std::min<uint32_t> (length - begin, (uint32_t) kMeasurementUnits);
		std::vector<double> waveform (window);
		for (uint32_t i = 0; i < window; ++i)
		{
			waveform[i] = (double) ((int) byteAt (start + begin + i) - kSampleBias);
		}

		const double cycle = estimateCycleLength (waveform, 0, 0);
		cycleCache[start] = cycle;
		return cycle;
	}

	void keyOn (Voice& voice, uint32_t time)
	{
		voice.keyed = true;
		++voice.keyOns;
		voice.latchStart = startOf (voice);
		voice.latchEnd = endOf (voice);
		voice.loops = (voice.control & kLoopFlag) == 0;

		voice.cycleLength = cycleFor (voice.latchStart, voice.latchEnd - voice.latchStart);
		voice.melodic = false;
		if (voice.cycleLength > 0.0 && voice.pitch != 0)
		{
			int fine = 0;
			const int note =
				quantiseFrequency (bytesPerSecond (voice) / voice.cycleLength, &fine);
			voice.melodic = note >= 0 && note <= kHighestMelodicNote;
		}

		// a looping voice is stopped by the control bit, not by its end address
		if (voice.loops || voice.pitch == 0 || voice.latchEnd <= voice.latchStart)
		{
			voice.gateEnd = 0;
		}
		else
		{
			const double seconds =
				(double) (voice.latchEnd - voice.latchStart) / bytesPerSecond (voice);
			voice.gateEnd = time + (uint32_t) std::llround (std::min (seconds * streamRate, 1.0e9));
		}
	}

	int volumeOf (const Voice& voice) const
	{
		const int level = std::max<int> (voice.volumeLeft & kVolumeMask, voice.volumeRight & kVolumeMask);
		return (level * 127 + 63) / 127;
	}

	bool sounding (const Voice& voice) const
	{
		if (! voice.keyed || volumeOf (voice) == 0)
		{
			return false;
		}

		return voice.gateEnd == 0 || now < voice.gateEnd;
	}
};

SegaPcmDecoder::SegaPcmDecoder()
	: impl (std::make_unique<Impl>())
{
}

SegaPcmDecoder::~SegaPcmDecoder() = default;

void SegaPcmDecoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
	// the ROM belongs to the song, not to the clock
	std::vector<uint8_t> rom = std::move (impl->rom);
	const uint32_t mask = impl->romMask;

	*impl = Impl();
	impl->rom = std::move (rom);
	impl->romMask = mask;
	impl->clock = clock > 0 ? (double) clock : 8000000.0;
	impl->streamRate = sampleRate > 0.0 ? sampleRate : 44100.0;

	// The chip slot's flags carry the VGM header's SegaPCM banking (shift in the low byte, mask in
	// the next); zero means the song said nothing and the format's pair stands.
	if (config.flags != 0)
	{
		impl->bankShift = (uint8_t) (config.flags & 0xFF);
		impl->bankMask = (uint8_t) ((config.flags >> 8) & 0xFF);
	}

	for (int i = 0; i < (int) SegaPcmDecoder::numChannels; ++i)
	{
		impl->tables[i] = std::make_unique<SegaPcmNoteTable> (impl->clock,
															  &impl->voices[i].cycleLength,
															  &impl->voices[i].melodic);
	}
}

void SegaPcmDecoder::setRom (std::vector<uint8_t> rom)
{
	impl->romMask = pow2Mask (rom.size());
	impl->rom = std::move (rom);
	impl->cycleCache.clear();
}

void SegaPcmDecoder::write (const RegisterWrite& write)
{
	// the command addresses the chip's register RAM: 8n + 0..7 is the low window, 0x80 + 8n + 0..7
	// the current address and control
	const uint16_t offset = (uint16_t) (((uint16_t) write.port << 8) | write.reg);
	const bool highWindow = (offset & kHighWindowBit) != 0;
	const int channel = (int) ((offset & kOffsetMask) >> kChannelShift);
	if (channel < 0 || channel >= (int) SegaPcmDecoder::numChannels)
	{
		return;
	}

	Impl::Voice& voice = impl->voices[channel];
	if (! highWindow)
	{
		switch (offset & kRegisterInWindowMask)
		{
		// One voice's eight registers; the start address is not among them, because it comes from
		// the command's address byte and is latched above. What the offsets mean:
		//
		//   0x02  volume, left        0x03  volume, right
		//   0x04/0x05  loop address, low byte then high, in 256-byte units
		//   0x06  end address, in 256-byte units - reaching it is what ends the voice
		//   0x07  pitch, where 128 plays the sample at its own rate
		case 0x02: voice.volumeLeft = write.value; break;
		case 0x03: voice.volumeRight = write.value; break;
		case 0x04: voice.loopLow = write.value; break;
		case 0x05: voice.loopHigh = write.value; break;
		case 0x06: voice.endReg = write.value; break;
		case 0x07: voice.pitch = write.value; break;
		default: break;
		}
		return;
	}

	switch (offset & kRegisterInWindowMask)
	{
	case 0x04:
	case 0x05:
	{
		// **0x84 is the middle byte and 0x85 the high one** - the core builds
		// `(0x85 << 16) | (0x84 << 8) | low` (`segapcm.c:157`). Testing `offset & 0x07` instead
		// of the low bit made both registers look like `addressHigh`, so `addressMid` only ever
		// came from the unused 0x80 and a sample starting at 0x437C drew from 0x4300. The shape
		// stayed plausible - a 256-byte-off copy of the same sample - which is why it survived.
		if ((offset & 0x01) != 0)
		{
			voice.addressHigh = write.value;
		}
		else
		{
			voice.addressMid = write.value;
		}

		// the address registers are also written to restart a voice that is already enabled: the
		// core has no key-on edge at all, so a driver looping past the end rewrites the address and
		// leaves the control bit clear. Without this, a loop plays its second pass silent.
		if ((voice.control & 0x01) == 0 && voice.keyed)
		{
			impl->keyOn (voice, write.sampleTime);
		}
		break;
	}
	case 0x06:
	{
		// bit 0 is the key: the core plays while it is clear, and a write that clears it after a
		// set one is the driver starting the voice
		const bool wasOff = (voice.control & 0x01) != 0;
		voice.control = write.value;
		if ((write.value & 0x01) == 0 && wasOff)
		{
			impl->keyOn (voice, write.sampleTime);
		}
		else if ((write.value & 0x01) != 0)
		{
			voice.keyed = false;
		}
		break;
	}
	default:
		break;
	}
}

void SegaPcmDecoder::advance (uint32_t sampleTime)
{
	impl->now = sampleTime;
}

ChannelState SegaPcmDecoder::channelState (int channel) const
{
	ChannelState state;

	if (channel < 0 || channel >= (int) SegaPcmDecoder::numChannels)
	{
		return state;
	}

	const Impl::Voice& voice = impl->voices[channel];
	state.present = true;
	state.gate = impl->sounding (voice);
	state.keyOns = voice.keyOns;
	state.period = voice.pitch;
	state.volume = impl->volumeOf (voice);

	const SampledPitch pitch = describeSamplePitch (impl->bytesPerSecond (voice),
													impl->referenceBytesPerSecond(),
													voice.cycleLength, voice.melodic);
	state.hz = pitch.hz;
	state.transposeCents = pitch.transposeCents;
	state.rootCents = pitch.rootCents;
	state.pitchSource = pitch.source;

	// **The window follows the registers, not the key-on.** The shape is in ROM - there is nothing
	// in the register state to synthesise one from - but *which* bytes is a live fact: the drivers
	// in this collection rebuild the current address while a voice plays (15 000 writes in
	// `01 Magical Sound Shower`) and rewrite the end page too (5 000), so a window latched at the
	// attack shows one region while the chip reads another. `docs/research/2026-11-12-segapcm-
	// registers-forensics.md` is where that was measured.
	//
	// `tableWave` stays false: this is a recording being played, not a cycle to loop.
	const uint32_t windowStart = impl->startOf (voice);
	const uint32_t windowEnd = impl->endOf (voice);

	if (impl->sounding (voice) && windowEnd > windowStart && ! impl->rom.empty ())
	{
		const uint32_t length = (std::min) (windowEnd - windowStart, kMaxWaveBytes);
		const uint64_t key = ((uint64_t) windowStart << 32) | (uint64_t) length;
		const auto found = impl->waveCache.find (key);

		if (found != impl->waveCache.end ())
		{
			state.wave = found->second;
		}
		else
		{
			auto wave = std::make_shared<std::vector<float>> ();
			wave->reserve (length);

			for (uint32_t i = 0; i < length; ++i)
			{
				wave->push_back ((float) ((int) impl->byteAt (windowStart + i) - kSampleBias) / 128.0f);
			}

			state.wave = wave;
			impl->waveCache.emplace (key, state.wave);
		}
	}

	// The rate the bytes are read at: one step of the 24-bit position is 1/256 of a byte, and the
	// chip renders at `clock / 128` - so `pitch * clock / 32768` bytes a second.
	if (voice.pitch != 0)
	{
		state.sampleRate = (float) impl->bytesPerSecond (voice);
	}

	return state;
}

const NoteTable& SegaPcmDecoder::noteTable (int channel) const
{
	const std::size_t index =
		(channel >= 0 && channel < (int) SegaPcmDecoder::numChannels) ? (std::size_t) channel : 0;
	return *impl->tables[index];
}

ChannelKind SegaPcmDecoder::channelKind (int channel) const
{
	return isDrum (channel) ? ChannelKind::adpcmDrum : ChannelKind::segaPcm;
}

bool SegaPcmDecoder::isDrum (int channel) const
{
	if (channel < 0 || channel >= (int) SegaPcmDecoder::numChannels)
	{
		return true;
	}

	return ! impl->voices[channel].melodic;
}

} // namespace toybox::chips
