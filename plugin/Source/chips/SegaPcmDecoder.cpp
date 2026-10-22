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

	/** The mask the ROM is folded with: the chip addresses `bank | (addr >> 8)` into a power-of-two
		buffer, so a sample that runs off the end wraps rather than reading elsewhere. */
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

		int noteFor (std::uint32_t pitch, int, int* finePitch) const override
		{
			if (pitch == 0 || melodic == nullptr || ! *melodic || cycleLength == nullptr
				|| *cycleLength <= 0.0)
			{
				*finePitch = 0;
				return 60;		// a drum: the C4 trigger
			}

			// L1: the step register against the chip's nominal rate, never the sample's bytes
			return quantisePlaybackRate ((double) pitch * bytesPerSecondPerStep, rootRate, finePitch);
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
		std::uint8_t volumeLeft = 0;
		std::uint8_t volumeRight = 0;
		std::uint8_t loopLow = 0;
		std::uint8_t loopHigh = 0;
		std::uint8_t endReg = 0;
		std::uint8_t pitch = 0;

		// the high window (0x80 + 8n + 0..7): current address and control
		std::uint8_t addressMid = 0;
		std::uint8_t addressHigh = 0;
		std::uint8_t control = 0xFF;

		bool keyed = false;
		std::uint32_t latchStart = 0;
		std::uint32_t latchEnd = 0;
		bool loops = false;
		std::uint32_t gateEnd = 0;
		double cycleLength = 0.0;
		bool melodic = false;
		std::uint32_t keyOns = 0;
	};

	Voice voices[SegaPcmDecoder::numChannels];
	std::vector<std::uint8_t> rom;
	std::uint32_t romMask = 0;
	double clock = 8000000.0;
	double streamRate = 44100.0;
	std::map<std::uint32_t, double> cycleCache;
	std::unique_ptr<SegaPcmNoteTable> tables[SegaPcmDecoder::numChannels];
	std::uint32_t now = 0;

	std::uint8_t byteAt (std::uint32_t address) const
	{
		const std::uint32_t index = address & romMask;
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

	/** Where the voice starts, in bytes: the 24-bit position's low byte is the chip's own fraction,
		so the address byte is `(addressHigh << 8) | addressMid`. */
	static std::uint32_t startOf (const Voice& voice)
	{
		return ((std::uint32_t) voice.addressHigh << 8) | voice.addressMid;
	}

	/** The end register counts 256-byte units and the core compares `addr >> 16`, so the sample ends
		at `(reg + 1) * 256`. */
	static std::uint32_t endOf (const Voice& voice)
	{
		return (std::uint32_t) (voice.endReg + 1) << 8;
	}

	double cycleFor (std::uint32_t start, std::uint32_t length)
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
		const std::uint32_t begin = length / 4;
		const std::uint32_t window =
			std::min<std::uint32_t> (length - begin, (std::uint32_t) kMeasurementUnits);
		std::vector<double> waveform (window);
		for (std::uint32_t i = 0; i < window; ++i)
		{
			waveform[i] = (double) ((int) byteAt (start + begin + i) - 0x80);
		}

		const double cycle = estimateCycleLength (waveform, 0, 0);
		cycleCache[start] = cycle;
		return cycle;
	}

	void keyOn (Voice& voice, std::uint32_t time)
	{
		voice.keyed = true;
		++voice.keyOns;
		voice.latchStart = startOf (voice);
		voice.latchEnd = endOf (voice);
		voice.loops = (voice.control & 0x02) == 0;

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
			voice.gateEnd = time + (std::uint32_t) std::llround (std::min (seconds * streamRate, 1.0e9));
		}
	}

	int volumeOf (const Voice& voice) const
	{
		const int level = std::max<int> (voice.volumeLeft & 0x7F, voice.volumeRight & 0x7F);
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

void SegaPcmDecoder::reset (std::uint32_t clock, double sampleRate)
{
	// the ROM belongs to the song, not to the clock
	std::vector<std::uint8_t> rom = std::move (impl->rom);
	const std::uint32_t mask = impl->romMask;

	*impl = Impl();
	impl->rom = std::move (rom);
	impl->romMask = mask;
	impl->clock = clock > 0 ? (double) clock : 8000000.0;
	impl->streamRate = sampleRate > 0.0 ? sampleRate : 44100.0;

	for (int i = 0; i < (int) SegaPcmDecoder::numChannels; ++i)
	{
		impl->tables[i] = std::make_unique<SegaPcmNoteTable> (impl->clock,
															  &impl->voices[i].cycleLength,
															  &impl->voices[i].melodic);
	}
}

void SegaPcmDecoder::setRom (std::vector<std::uint8_t> rom)
{
	impl->romMask = pow2Mask (rom.size());
	impl->rom = std::move (rom);
	impl->cycleCache.clear();
}

void SegaPcmDecoder::write (const RegisterWrite& write)
{
	// the command addresses the chip's register RAM: 8n + 0..7 is the low window, 0x80 + 8n + 0..7
	// the current address and control
	const std::uint16_t offset = (std::uint16_t) (((std::uint16_t) write.port << 8) | write.reg);
	const bool highWindow = (offset & 0x80) != 0;
	const int channel = (int) ((offset & 0x7F) >> 3);
	if (channel < 0 || channel >= (int) SegaPcmDecoder::numChannels)
	{
		return;
	}

	Impl::Voice& voice = impl->voices[channel];
	if (! highWindow)
	{
		switch (offset & 0x07)
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

	switch (offset & 0x07)
	{
	case 0x04:
	case 0x05:
	{
		if (offset & 0x07)
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

void SegaPcmDecoder::advance (std::uint32_t sampleTime)
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

	return state;
}

const NoteTable& SegaPcmDecoder::noteTable (int channel) const
{
	const std::size_t index =
		(channel >= 0 && channel < (int) SegaPcmDecoder::numChannels) ? (std::size_t) channel : 0;
	return *impl->tables[index];
}

Family SegaPcmDecoder::family (int channel) const
{
	return isDrum (channel) ? Family::adpcmDrum : Family::segaPcmPcm;
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
