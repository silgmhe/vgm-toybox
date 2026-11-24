#include "OplDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "OpnAdpcmB.h"

#include "Pitch.h"

namespace toybox::chips
{
namespace
{
	// A rhythm instrument or a Delta-T sample has no pitch of its own: C4 marks the hit.
	constexpr int kOneShotNote = 60;
	constexpr uint32_t kOneShotPeriod = 0x80000000u;

	constexpr uint32_t kRhythmGate = 4410;		// 100 ms at 44.1 kHz
	constexpr uint32_t kAdpcmGate = 5292;		// 120 ms

	/** A row draws a shape, not a recording: the Y8950's ranges are 4 KB each on this collection
		(8 192 nibbles), and a driver that points `stop` at the end of the ROM must not turn one row
		into a 2 MB decode. Same ceiling the OPN's Delta-T panel uses. */
	constexpr std::size_t kAdpcmWindowBytes = 4096;

	/** The OPL register map groups its channels in runs of nine: 0x10..0x18 carry each channel's
		frequency low byte, 0x20..0x28 the same channel's block and F-number high bit, 0xB0..0xB8
		its key-on and feedback. So a channel index is the register minus the run's base, and the
		run length is what bounds a write to "the first nine channels only". */
	constexpr uint8_t kFnumLowBase = 0x10;
	constexpr uint8_t kBlockBase = 0x20;
	constexpr uint8_t kKeyOnBase = 0xB0;
	constexpr uint8_t kChannelRun = 9;

	/** Which operator a 0x20-range register addresses, indexed by the register's low five bits.

		Taken from libvgm's own OPL core (libs/libvgm/emu/cores/fmopl.c:284), not from the register
		map by eye: the sequence is 0,2,4 / 1,3,5 / ... so that operators 0 and 1 belong to channel 0
		and 2 and 3 to channel 1 - even numbers are the modulators. Treating the register index as a
		channel number would read a neighbour's multiplier and draw a patch that belongs to another
		instrument, with nothing to indicate it. */
	constexpr int kOperatorOfRegister[32] =
	{
		 0,  2,  4,  1,  3,  5, -1, -1,
		 6,  8, 10,  7,  9, 11, -1, -1,
		12, 14, 16, 13, 15, 17, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1
	};

	/** The frequency is nine bits: eight in the low register and the ninth above the block. */
	constexpr uint16_t kFnumHighBit = 0x100;
	constexpr uint16_t kFnumLowMask = 0x0FF;
	constexpr int kFnumHighShift = 8;

	/** OPL3 widens the frequency to ten bits, so its high register carries two of them. */
	constexpr uint8_t kFnumHighOneBit = 0x01;
	constexpr uint8_t kFnumHighTwoBits = 0x03;

	/** A register operand is five bits wide, and Delta-T's start bit sits in the top of its own
		control byte. */
	constexpr uint8_t kOperandMask = 0x1F;
	constexpr uint8_t kDeltaTStart = 0x80;

	class OplNoteTable final : public NoteTable
	{
	public:
		void configure (double chipClock, double clockDivisor, int octaveReference)
		{
			clock = chipClock > 0.0 ? chipClock : 3579545.0;
			divisor = clockDivisor;
			reference = octaveReference;
		}

		int noteFor (uint32_t fnum, int block, int* finePitch) const override
		{
			if ((fnum & kOneShotPeriod) != 0)
			{
				*finePitch = 0;
				return kOneShotNote;
			}

			if (fnum == 0)
			{
				*finePitch = 0;
				return -1;
			}

			const double hz = (double) fnum * clock / divisor
							  * std::exp2 ((double) block - (double) reference);
			return quantiseFrequency (hz, finePitch);
		}

	private:
		double clock = 3579545.0;
		double divisor = 72.0;
		int reference = 20;
	};
}

struct OplDecoder::Impl
{
	struct Channel
	{
		int fnum = 0;
		int block = 0;
		bool keyed = false;

		/** The OPLL's voice selection: `0x30 + ch`'s two halves.

			**The instrument number is the whole patch on this chip** - it is a pointer into the ROM
			table the core already carries, so `capturePatch` hands that byte back instead of copying
			the table (`docs/desgin/2026-11-22-opl-visualisation.md`). Zero means "the user voice",
			whose eight registers are global and live in `Impl::userPatch`. */
		uint8_t instrument = 0;

		/** **0 is the loudest**, the same way round as an OPN's total level: the byte is an
			attenuation, and `emu2413`'s `set_volume` adds it straight into the carrier's TL
			(`emu2413.c:679`). The panel wants the other direction, so `channelState` inverts it. */
		uint8_t volume = 0;

		/** **The chip's own operator registers, both operators, as the song wrote them.**

			`[operator][bank]`, where the banks are `0x20` (AM/VIB/EG/KSR/MULT), `0x40` (KSL/TL),
			`0x60` (AR/DR) and `0x80` (SL/RR). An OPL2 patch is these eight bytes plus
			`feedbackConnection`, which is what `capturePatch` hands back - unlike the OPLL, where a
			patch is a preset number.

			The envelope lives in banks 2 and 3, and **neither was read before this**: a patch built
			from the two multifplier/level bytes alone has a zero attack rate and renders as silence.
		*/
		uint8_t opRegs[2][4] = {};

		uint8_t feedbackConnection = 0;		// 0xC0: feedback bits 1-3, connection bit 0

		/** Bumped by every write that changes the patch, so `NoteCache` can tell when it is stale. */
		uint32_t patchVersion = 0;

		/** The modulator's ratio and level, kept for the packed `fmKnown` shape and for nothing else. */
		int multiple = 1;		// 0x20 + slot, low four bits
		uint8_t totalLevel = 0x3F;	// 0x40 + slot, low six bits
		int feedback = 0;		// 0xC0 + channel, bits 1-3
		int waveform = 0;		// 0xE0 + slot, low two bits (OPL3; OPL2 has one sine)
	};

	/** A level-triggered hit: the rising edge sets the end time, the chip itself ends it. */
	struct Pulse
	{
		uint32_t end = 0;
	};

	Variant variant = Variant::opl2;
	bool adpcm = false;

	/** The OPLL's eight global registers for instrument 0 - `0x00..0x07`, in chip order. */
	uint8_t userPatch[8] = {};

	/** **The rhythm voices' parameters, by low nibble**: `0x16-0x18` instrument, `0x26-0x28`
		block/f-number, `0x36-0x38` level. They are the same addresses the sixth to eighth melodic
		channels use, so they are kept separately: while rhythm mode is on those channels are drums
		and their melodic fields mean nothing. */
	uint8_t rhythmRegs[16] = {};
	uint32_t rhythmVersion = 0;

	/** Bumped whenever those eight change, so a cached patch for an `instrument 0` channel knows it
		is stale. Nothing else can change a user voice. */
	uint32_t userPatchVersion = 0;

	/** **The patch the panel draws from**, handed out by `channelState` the way `OpnDecoder` does.

		`capturePatch` is cheap on this chip, but `channelState` runs thirty times a second per row
		and each capture allocates a `shared_ptr`, so the result is cached against the three values
		it depends on. **Without this the panel had no patch at all** and every OPLL row fell back to
		the synthetic single cycle - the shape this file's owner described as "one period, the old
		scheme", on rows whose notes and keys were already correct.
	*/
	struct NoteCache
	{
		bool valid = false;

		/** The channel's own `patchVersion`, and the global user-voice one. The second only matters
			on an OPLL channel using instrument 0, where the eight global registers are part of the
			answer; everywhere else it stays zero and never invalidates anything. */
		uint32_t version = 0;
		uint32_t userVersion = 0;

		std::shared_ptr<const VoicePatch> patch;
	};

	NoteCache noteCache[18];

	/** Which model a captured patch names: the OPLL is the only one this decoder renders today. */
	ChipModel patchModel (void) const
	{
		return variant == Variant::opll ? ChipModel::ym2413 : ChipModel::ym3812;
	}

	Channel channels[18];
	Pulse rhythm[5];
	Pulse deltaT;

	/** **The Y8950's Delta-T state**: a sample player rather than a modulator. The ROM has no
		addresses of its own in VGM - `0x88` is one block - so the registers below are the whole map,
		and they are what the panel and the exporter both read. */
	std::vector<uint8_t> rom;
	std::shared_ptr<const std::vector<float>> adpcmWave;
	double clock = 3579545.0;
	uint8_t adpcmMode = 0;		// 0x08: the low two bits pick the address granularity
	uint8_t startLow = 0;
	uint8_t startHigh = 0;
	uint8_t stopLow = 0;
	uint8_t stopHigh = 0;
	uint16_t deltaN = 0;		// 0x10 / 0x11
	uint8_t level = 0;			// 0x12

	/** **CPU-fed Delta-T.** Control 0x07 bit 5 selects where the bytes come from: set means external
		memory (the `0x88` image, addressed by the start/stop registers), **clear means the driver
		writes them to 0x0F** and the unit synthesises as they arrive (`ymdeltat.c:169-171`).

		There is no address in that mode, so the row shows what was written since START - the same
		"buffer, no table" shape the YM2612 DAC row has, decoded through the same delta table.
	*/
	bool adpcmCpuMode = false;
	std::vector<uint8_t> adpcmCpuBytes;

	/** Delta-T's address shift, from the core's `ymdeltat.c`: a page is 32 bytes, and a control2 whose
		low two bits are 0 selects a part with four-byte granularity (`dram_rightshift[0] = 3`). */
	int adpcmShift (void) const
	{
		static constexpr int kDramRightShift[4] = { 3, 0, 0, 0 };
		return 5 - kDramRightShift[adpcmMode & 3];
	}

	/** Nibbles a second the unit is clocked at: `delta * clock / (72 * 65536)`.

		The **72**, not the 144 the OPN family uses. `fmopl.c` sets `freqbase = (clock / 72) / rate`
		and `ymdeltat.c` consumes one nibble per overflow of the 16.16 accumulator, so a 3.58 MHz
		Y8950 at DELTA-N 0x51F0 runs at 15 912 Hz while an 8 MHz OPN at 0x4000 runs at 13 889. Using
		the OPN divisor here would export every Y8950 sample at half speed.
	*/
	double adpcmBytesPerSecond (void) const
	{
		return (double) deltaN * clock / (72.0 * 65536.0);
	}

	/** Decodes the range the registers name, once per key-on rather than once per snapshot. */
	void latchAdpcm (void)
	{
		adpcmWave = nullptr;

		if (deltaN == 0 || rom.empty ())
		{
			return;
		}

		const int shift = adpcmShift ();
		const uint32_t start = (uint32_t) ((startHigh << 8) | startLow) << shift;
		const uint32_t last = (((uint32_t) ((stopHigh << 8) | stopLow)) << shift)
							  | ((1u << shift) - 1u);

		if (last < start || start >= rom.size ())
		{
			return;
		}

		const std::size_t length = (std::min<std::size_t>) (last - start + 1u, rom.size () - start);
		const std::size_t capped = (std::min<std::size_t>) (length, kAdpcmWindowBytes);

		auto wave = std::make_shared<std::vector<float>> ();
		opn::deltaT::decode (rom.data () + start, capped, *wave);
		adpcmWave = std::move (wave);
	}
	uint8_t rhythmBits = 0;
	bool rhythmEnabled = false;
	uint32_t now = 0;
	OplNoteTable table;

	int channelCount = 14;
	int rhythmBase = 9;
	bool twoBanks = false;

	void configure (void)
	{
		switch (variant)
		{
		case Variant::opll:
			channelCount = 14;
			rhythmBase = 9;
			twoBanks = false;
			table.configure (0.0, 72.0, 19);
			break;

		case Variant::opl2:
			channelCount = adpcm ? 15 : 14;
			rhythmBase = 9;
			twoBanks = false;
			table.configure (0.0, 72.0, 20);
			break;

		case Variant::opl3:
		default:
			channelCount = 23;
			rhythmBase = 18;
			twoBanks = true;
			table.configure (0.0, 288.0, 20);
			break;
		}
	}

	/** Rhythm mode borrows channels 6..8 of the first bank; entering it stops whatever those
		channels still held, exactly as the chip does. */
	void rhythmWrite (uint32_t time, uint8_t data, bool enabled)
	{
		if (! enabled)
		{
			rhythmEnabled = false;
			rhythmBits = 0;
			return;
		}

		if (! rhythmEnabled)
		{
			rhythmEnabled = true;
			rhythmBits = 0;
			for (int channel = 6; channel < 9; ++channel)
			{
				channels[channel].keyed = false;
			}
		}

		const uint8_t bits = (uint8_t) (data & kOperandMask);
		const uint8_t rising = (uint8_t) (bits & (uint8_t) ~rhythmBits);
		for (int i = 0; i < 5; ++i)
		{
			if ((rising & (0x10u >> i)) != 0)
			{
				rhythm[i].end = time + kRhythmGate;
			}
		}
		rhythmBits = bits;
	}

	void keyChannel (int channel, bool keyOn)
	{
		if (channel < 0 || channel >= channelCount)
		{
			return;
		}
		channels[channel].keyed = keyOn;
	}
};

OplDecoder::OplDecoder (Variant variant, bool adpcm) : impl (std::make_unique<Impl>())
{
	impl->variant = variant;
	impl->adpcm = adpcm;
	impl->configure();
}

OplDecoder::~OplDecoder() = default;

void OplDecoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;

	// **The replayer makes decoders with the default constructor**, so the device has to say which
	// OPL it is the same way the OPN family does: through `ChipConfig::flags`. Bit 0 says the Y8950
	// (whose Delta-T unit gives the chip a fifteenth row); bits 1-2 carry the variant, because the
	// default is `opl2` and three kinds share this decoder.
	//
	// **Zero in those bits means "the constructor already said"**, which keeps the callers that
	// build a decoder with a variant - `MidiSequenceBuilder`, and the tests - working unchanged.
	const uint32_t flaggedVariant = (config.flags >> 1) & 3u;
	const Variant variant = (flaggedVariant != 0) ? (Variant) (flaggedVariant - 1) : impl->variant;

	const bool adpcm = impl->adpcm || (config.flags & deltaTFlag) != 0;
	*impl = Impl();
	impl->variant = variant;
	impl->adpcm = adpcm;
	impl->clock = clock > 0 ? (double) clock : 3579545.0;
	impl->configure();

	switch (variant)
	{
	case Variant::opll:
		impl->table.configure ((double) (clock > 0 ? clock : 3579545u), 72.0, 19);
		break;
	case Variant::opl2:
		impl->table.configure ((double) (clock > 0 ? clock : 3579545u), 72.0, 20);
		break;
	case Variant::opl3:
	default:
		impl->table.configure ((double) (clock > 0 ? clock : 14318180u), 288.0, 20);
		break;
	}
}

void OplDecoder::attachMemory (const ChipMemory& memory)
{
	if (memory.bytes != nullptr && ! memory.bytes->empty ())
	{
		impl->rom = *memory.bytes;
	}
}

void OplDecoder::write (const RegisterWrite& write)
{
	const uint8_t reg = write.reg;
	const int port = write.port != 0 ? 1 : 0;
	const uint32_t time = write.sampleTime;

	// **Any write while the OPL2's rhythm is on can change a drum.** The five rows cache their patch
	// against this counter; without the bump they kept whatever was there when the row was first
	// drawn, which on this path was a patch assembled from untouched registers.
	if (impl->rhythmEnabled && impl->variant != Variant::opll)
	{
		++impl->rhythmVersion;
	}

	if (impl->adpcm && port == 0 && reg >= 0x07 && reg <= 0x12)
	{
		// **Delta-T's whole register block, which is a sample player**: `0x08` picks the address
		// granularity, `0x09/0x0A` and `0x0B/0x0C` the start and the last page, `0x10/0x11` the
		// pitch and `0x12` the level. `0x07` bits 7 starts a sample; the unit ends it at the stop
		// address, which the exporter reads instead of this pulse.
		switch (reg)
		{
		case 0x07:
		{
			// **Bit 5 says where the bytes are.** Clear: the driver writes them to 0x0F and the unit
			// synthesises as they arrive; set: external memory, addressed by 0x09-0x0C.
			const bool start = (write.value & kDeltaTStart) != 0;
			const bool external = (write.value & 0x20) != 0;

			impl->adpcmCpuMode = start && ! external;

			if (start)
			{
				if (impl->deltaT.end <= time)
				{
					impl->deltaT.end = time + kAdpcmGate;
				}

				if (impl->adpcmCpuMode)
				{
					// A new stream: the previous hit's bytes are not this one's.
					impl->adpcmCpuBytes.clear ();
					impl->adpcmWave = nullptr;
				}
				else
				{
					impl->latchAdpcm ();
				}
			}

			break;
		}

		case 0x0F:
		{
			// The CPU-fed data port. Each byte is two nibbles and the row draws them as they land,
			// re-decoding the buffer because the delta codec is stateful: there is no way to decode
			// one more byte without the ones before it.
			if (! impl->adpcmCpuMode || impl->adpcmCpuBytes.size () >= kAdpcmWindowBytes)
			{
				break;
			}

			impl->adpcmCpuBytes.push_back ((uint8_t) write.value);

			auto wave = std::make_shared<std::vector<float>> ();
			opn::deltaT::decode (impl->adpcmCpuBytes.data (), impl->adpcmCpuBytes.size (), *wave);
			impl->adpcmWave = std::move (wave);

			if (impl->deltaT.end <= time)
			{
				impl->deltaT.end = time + kAdpcmGate;
			}

			break;
		}

		case 0x08: impl->adpcmMode = write.value; break;
		case 0x09: impl->startLow = write.value; break;
		case 0x0A: impl->startHigh = write.value; break;
		case 0x0B: impl->stopLow = write.value; break;
		case 0x0C: impl->stopHigh = write.value; break;

		case 0x10:
			impl->deltaN = (uint16_t) ((impl->deltaN & 0xFF00u) | write.value);
			break;

		case 0x11:
			impl->deltaN = (uint16_t) ((impl->deltaN & 0x00FFu) | ((uint16_t) write.value << 8));
			break;

		case 0x12: impl->level = write.value; break;
		default: break;
		}

		return;
	}

	if (impl->variant == Variant::opll)
	{
		// **The rhythm voices' three parameters each.** Kept here rather than in the channel fields
		// because these addresses belong to channels 6-8, which the drums borrow.
		//
		// **Captured whether or not rhythm mode is on yet.** A driver may set the instruments first
		// and enable rhythm afterwards - this song does - and gating on `rhythmEnabled` left every
		// value at zero, which rendered as a silent patch.
		if ((reg >= 0x16 && reg <= 0x18) || (reg >= 0x26 && reg <= 0x28)
			|| (reg >= 0x36 && reg <= 0x38))
		{
			impl->rhythmRegs[reg & 0x0F] = write.value;
			++impl->rhythmVersion;
		}

		// **The user voice, which is eight global registers.** An OPLL has no per-channel operator
		// registers, but it does have this: instruments numbered 0 use these eight bytes, and any
		// channel can ask for them. `capturePatch` copies them so a song that never uses a preset
		// still gets its own timbre instead of a generic sine.
		if (reg < 8)
		{
			impl->userPatch[reg] = write.value;
			++impl->userPatchVersion;
			return;
		}

		if (reg >= kFnumLowBase && reg < kFnumLowBase + kChannelRun)
		{
			Impl::Channel& voice = impl->channels[reg - kFnumLowBase];
			voice.fnum = (voice.fnum & kFnumHighBit) | (int) write.value;
			return;
		}

		if (reg >= kBlockBase && reg < kBlockBase + kChannelRun)
		{
			const int channel = reg - kBlockBase;
			if (impl->rhythmEnabled && channel >= 6)
			{
				return;		// borrowed by the rhythm instruments
			}

			Impl::Channel& voice = impl->channels[channel];
			voice.fnum = (voice.fnum & kFnumLowMask) | ((int) (write.value & kFnumHighOneBit) << kFnumHighShift);
			voice.block = (int) ((write.value >> 1) & 0x07);
			voice.keyed = (write.value & 0x10) != 0;
			return;
		}

		if (reg >= 0x30 && reg < 0x39)
		{
			// bit 5 is the key-off; the rest of the register is instrument and volume
			if ((write.value & 0x20) != 0)
			{
				impl->keyChannel (reg - 0x30, false);
			}

			// **Both halves are kept, because `capturePatch` needs the byte back.** The high nibble
			// selects one of the chip's ROM instruments (or 0 for the eight registers below), and the
			// low nibble is this channel's volume - which `channelState` used to report as a constant
			// fifteen, so every OPLL row's meter was full whatever the song asked for.
			Impl::Channel& voice = impl->channels[reg - 0x30];
			voice.instrument = (uint8_t) ((write.value >> 4) & 0x0F);
			voice.volume = (uint8_t) (write.value & 0x0F);
			++voice.patchVersion;
			return;
		}

		if (reg == 0x0E)
		{
			impl->rhythmWrite (time, write.value, (write.value & 0x20) != 0);
		}
		return;
	}

	// The patch registers are addressed by slot, not by channel. **Both operators are kept now**:
	// the modulator's ratio and level make this sound unlike its neighbour, and the carrier's
	// attack, decay, sustain and release are the envelope the row draws - a patch with only half of
	// it renders as silence, because a zero attack rate never starts.
	const bool twoBanks = impl->twoBanks;
	const auto modulatorChannel = [twoBanks, port] (uint8_t patchRegister) -> int
	{
		const int op = kOperatorOfRegister[patchRegister & 0x1F];
		if (op < 0 || (op & 1) != 0)
		{
			return -1;		// an unused window, or the carrier
		}

		return (twoBanks ? port * 9 : 0) + op / 2;
	};

	/** The slot an operator register belongs to: `index` is 0..17 with the modulator even, so the
		channel is `index / 2` within its bank and the operator is `index & 1`. */
	const auto operatorSlot = [twoBanks, port] (uint8_t patchRegister, int& channelOut,
												int& opOut) -> bool
	{
		const int index = kOperatorOfRegister[patchRegister & 0x1F];
		if (index < 0)
		{
			return false;		// one of the chip's six unused windows
		}

		channelOut = (twoBanks ? port * 9 : 0) + index / 2;
		opOut = index & 1;
		return true;
	};

	/** The four register banks in the order `Impl::Channel::opRegs` keeps them. */
	const auto keepOperatorRegister = [&operatorSlot, impl = impl.get (), &write] (uint8_t base,
																					int block) -> bool
	{
		int channel = -1;
		int op = -1;

		if (! operatorSlot ((uint8_t) (base + (write.reg & 0x1F)), channel, op))
		{
			return false;
		}

		Impl::Channel& voice = impl->channels[channel];
		voice.opRegs[op][block] = write.value;
		++voice.patchVersion;
		return true;
	};

	// 0x20-0x35: AM/VIB/EG/KSR/MULTI, the multiplier in the low four bits. The range stops at 0x36
	// because that is where the eighteen slots end - `kOperatorOfRegister` is indexed by
	// `reg & 0x1F`, so a wider range would fold 0x38 back onto slot 0x18 and write channel 6's
	// multiplier from a register the chip does not define.
	if (reg >= 0x20 && reg < 0x36)
	{
		if (keepOperatorRegister (0x20, 0))
		{
			const int channel = modulatorChannel (reg);
			if (channel >= 0)
			{
				impl->channels[channel].multiple = (int) (write.value & 0x0F);
			}
		}
		return;
	}

	// 0x40-0x55: KSL and total level, six bits of attenuation
	if (reg >= 0x40 && reg < 0x56)
	{
		if (keepOperatorRegister (0x40, 1))
		{
			const int channel = modulatorChannel (reg);
			if (channel >= 0)
			{
				impl->channels[channel].totalLevel = (uint8_t) (write.value & 0x3F);
			}
		}
		return;
	}

	// **0x60-0x75 and 0x80-0x95 were not read at all before this**, so every captured patch would
	// have had a zero attack rate on both operators - a voice that never starts. They hold AR/DR and
	// SL/RR, the two halves of the envelope the row draws.
	if (reg >= 0x60 && reg < 0x76)
	{
		keepOperatorRegister (0x60, 2);
		return;
	}

	if (reg >= 0x80 && reg < 0x96)
	{
		keepOperatorRegister (0x80, 3);
		return;
	}

	// 0xC0-0xC8: feedback in bits 1-3, connection in bit 0
	if (reg >= 0xC0 && reg < 0xC9)
	{
		const int channel = (impl->twoBanks ? port * 9 : 0) + (reg - 0xC0);
		impl->channels[channel].feedback = (int) ((write.value >> 1) & 0x07);
		impl->channels[channel].feedbackConnection = write.value;
		++impl->channels[channel].patchVersion;
		return;
	}

	// 0xE0-0xF5: the operator's waveform select, same eighteen slots. OPL2 has no such register -
	// the write is harmless and never read - while OPL3 uses it for four choices.
	if (reg >= 0xE0 && reg < 0xF6)
	{
		const int channel = modulatorChannel (reg);
		if (channel >= 0)
		{
			impl->channels[channel].waveform = (int) (write.value & 0x03);
		}
		return;
	}

	if (reg >= 0xA0 && reg < 0xA9)
	{
		const int channel = (impl->twoBanks ? port * 9 : 0) + (reg - 0xA0);
		Impl::Channel& voice = impl->channels[channel];
		voice.fnum = (voice.fnum & 0x300) | (int) write.value;
		return;
	}

	if (reg >= 0xB0 && reg < 0xB9)
	{
		const int channel = (impl->twoBanks ? port * 9 : 0) + (reg - 0xB0);

		// **The borrowed channels still have a pitch, and the drums need it.** `0xB6` and `0xB8` carry
		// the bass drum's and the tom's block and f-number. Kept whether or not rhythm mode is on yet:
		// a driver may write the pitch before enabling, and discarding it left both at block 0 - a
		// DC step instead of a hit, which is the failure measurement found on this path.
		if (impl->rhythmEnabled && port == 0 && channel >= 6 && channel < 9)
		{
			Impl::Channel& borrowed = impl->channels[channel];
			borrowed.fnum = (borrowed.fnum & kFnumLowMask)
							| ((int) (write.value & kFnumHighTwoBits) << kFnumHighShift);
			borrowed.block = (int) ((write.value >> 2) & 0x07);
			borrowed.keyed = (write.value & 0x20) != 0;		// melodic 6-8 still need this
			return;
		}

		Impl::Channel& voice = impl->channels[channel];
		voice.fnum = (voice.fnum & kFnumLowMask) | ((int) (write.value & kFnumHighTwoBits) << kFnumHighShift);
		voice.block = (int) ((write.value >> 2) & 0x07);
		voice.keyed = (write.value & 0x20) != 0;
		return;
	}

	if (reg == 0xBD && port == 0)
	{
		impl->rhythmWrite (time, write.value, (write.value & 0x20) != 0);
	}
}

void OplDecoder::advance (uint32_t sampleTime)
{
	impl->now = sampleTime;
}

int OplDecoder::channelCount() const
{
	return impl->channelCount;
}

ChannelState OplDecoder::channelState (int channel) const
{
	ChannelState state;
	if (channel < 0 || channel >= impl->channelCount)
	{
		return state;
	}

	const int melodic = impl->twoBanks ? 18 : 9;
	if (channel < melodic)
	{
		const Impl::Channel& voice = impl->channels[channel];
		state.present = true;
		state.period = (uint32_t) voice.fnum;
		state.block = voice.block;
		state.gate = voice.keyed;

		// **The channel's own volume, and inverted.** Both families store an attenuation -
		// `0x30`'s low nibble on an OPLL, the carrier's total level on an OPL2 - while
		// `ChannelFromChip` normalises against `volumeMaximum = 15` where 15 means full. Reporting a
		// constant fifteen made every row read full whatever the song asked for, which is what this
		// did before either family had a level to report.
		if (impl->variant == Variant::opll)
		{
			state.volume = 15 - (int) voice.volume;
		}
		else
		{
			const int carrierLevel = voice.opRegs[1][1] & 0x3F;		// 0 loudest, 63 silent
			state.volume = 15 - (carrierLevel * 15 + 31) / 63;
		}

		// The OPLL has no per-operator registers - its instruments are pre-sets chosen by number -
		// so it says nothing rather than saying "multiplier 1, level 0", which is a patch that
		// belongs to some other chip.
		if (impl->variant != Variant::opll)
		{
			state.fmKnown = true;
			state.fmMultiple = voice.multiple;
			state.fmLevel = voice.totalLevel;		// already 0..63, the packed form's range
			state.fmFeedback = voice.feedback;
			state.fmWaveform = voice.waveform;
		}

		// **The patch the panel renders a note from**, handed out the way `OpnDecoder` does. A row's
		// shape is the whole note - attack, decay, release - and it only exists if the caller is
		// given a patch to render; without one the row falls back to a single synthetic cycle, which
		// is what both families did before this was here.
		{
			Impl::NoteCache& cache = impl->noteCache[channel];
			const uint32_t userVersion
				= (impl->variant == Variant::opll && voice.instrument == 0) ? impl->userPatchVersion : 0u;

			if (! cache.valid || cache.version != voice.patchVersion
				|| cache.userVersion != userVersion)
			{
				cache.valid = true;
				cache.version = voice.patchVersion;
				cache.userVersion = userVersion;

				VoicePatch captured;
				cache.patch = capturePatch (channel, captured)
								  ? std::make_shared<const VoicePatch> (std::move (captured))
								  : nullptr;
			}

			state.voicePatch = cache.patch;
		}

		return state;
	}

	if (channel >= impl->rhythmBase && channel < impl->rhythmBase + 5)
	{
		state.present = true;
		state.period = kOneShotPeriod;
		state.volume = 12;
		state.gate = impl->now < impl->rhythm[channel - impl->rhythmBase].end;

		// **The row needs the patch too.** Without it the panel falls back to a synthetic cycle that
		// belongs to no drum, which is what these five rows drew before this was here.
		if (impl->variant == Variant::opll)
		{
			Impl::NoteCache& cache = impl->noteCache[channel];

			if (! cache.valid || cache.version != impl->rhythmVersion)
			{
				cache.valid = true;
				cache.version = impl->rhythmVersion;
				cache.userVersion = 0;

				VoicePatch captured;
				cache.patch = capturePatch (channel, captured)
								  ? std::make_shared<const VoicePatch> (std::move (captured))
								  : nullptr;
			}

			state.voicePatch = cache.patch;
		}
		else
		{
			VoicePatch captured;

			if (capturePatch (channel, captured))
			{
				state.voicePatch = std::make_shared<const VoicePatch> (std::move (captured));
			}
		}

		return state;
	}

	if (impl->adpcm && channel == 14)
	{
		state.present = true;
		state.period = kOneShotPeriod;
		state.volume = 12;
		state.gate = impl->now < impl->deltaT.end;

		// **The row can point at a sample, unlike the YM2612's DAC.** The range is named by two
		// registers, so what the row draws is the same bytes the exporter writes; the rate is the
		// pitch register over the OPL's own time base.
		if (impl->adpcmWave != nullptr)
		{
			state.wave = impl->adpcmWave;
			state.sampleRate = (float) impl->adpcmBytesPerSecond ();
		}
	}

	return state;
}

const NoteTable& OplDecoder::noteTable (int) const
{
	return impl->table;
}

ChannelKind OplDecoder::channelKind (int channel) const
{
	const int melodic = impl->twoBanks ? 18 : 9;
	if (channel < melodic)
	{
		return ChannelKind::fm;
	}

	if (channel >= impl->rhythmBase && channel < impl->rhythmBase + 5)
	{
		return ChannelKind::fmDrum;
	}

	return impl->adpcm && channel == 14 ? ChannelKind::adpcmDrum : ChannelKind::unknown;
}

bool OplDecoder::isDrum (int channel) const
{
	const int melodic = impl->twoBanks ? 18 : 9;
	if (channel < melodic)
	{
		return false;
	}

	return (channel >= impl->rhythmBase && channel < impl->rhythmBase + 5)
		   || (impl->adpcm && channel == 14);
}

bool OplDecoder::capturePatch (int channel, VoicePatch& out) const
{
	const int melodic = impl->twoBanks ? 18 : 9;

	// **A rhythm voice is a one-shot with no pitch.** Its parameters are the nine registers the song
	// wrote in rhythm mode; the trigger is one bit of `0x0E`, so the driver never needs a frequency.
	//
	// The five are ordered BD, SD, TOM, CYM, HH from bit 4 down - `emu2413.c:616` `update_key_status`
	// is the table this mirrors.
	if (impl->variant == Variant::opll && channel >= impl->rhythmBase
		&& channel < impl->rhythmBase + 5)
	{
		out = VoicePatch {};
		out.model = ChipModel::ym2413;
		out.oneShot = true;
		out.triggerMask = (uint8_t) (0x10u >> (channel - impl->rhythmBase));

		// **Long enough to hold the whole hit, short enough to fill the row.** The bass drum's own
		// envelope was measured at about 41 ms; 150 leaves room for the slower voices without turning
		// the drawing into a spike at the left edge followed by a flat line.
		out.lengthSeconds = 0.15;

		// **Measured against a melodic note, because that is what a reader compares it to.** With
		// this song's own registers the hit peaks at `1.92` untrimmed; `0.5` puts it at `0.98`, which
		// is where a full-level melodic voice sits (`1.01`). `0.12` - the first guess - left it at
		// `0.235`, a quarter of a note, which reads as "the drum is missing" in a DAW.
		//
		// The chip's own level bytes carry the differences between the five voices; this constant only
		// puts the family where the notes are.
		out.trim = 0.5f;

		// **Six registers, read from the channel fields the melodic path already fills.** That path
		// receives `0x16-0x18` (f-number) and `0x26-0x28` (block and the ninth bit) as well as
		// `0x36-0x38` (instrument and volume), and a second store beside it did not.
		//
		// **The block is what makes a drum audible.** Without `0x26` the bass drum sits at block 0,
		// f-number 0 - measured `peak 0.0059`, i.e. a DC step rather than a hit - while the song
		// writes block 2 for it.
		for (int i = 0; i < 3; ++i)
		{
			const Impl::Channel& voice = impl->channels[impl->rhythmBase - 3 + i];

			out.regs.push_back ({ (uint16_t) (0x16 + i), (uint16_t) (voice.fnum & 0xFF) });
			out.regs.push_back ({ (uint16_t) (0x26 + i),
								  (uint16_t) (((voice.block & 0x07) << 1) | ((voice.fnum >> 8) & 0x01)) });
			out.regs.push_back ({ (uint16_t) (0x36 + i),
								  (uint16_t) ((voice.instrument << 4) | voice.volume) });
		}

		return true;
	}

	// **The OPL2 family's drums borrow channels 6-8.** Their parameters are those channels' operator
	// registers, addressed through the bank's offset windows - `regbase2modop`
	// (`adlibemu_opl_inc.c:89`) is the table, and the drum each window feeds is read off
	// `operator_advance_drums` (`:1439`): BD is ch6's two operators, SD ch6's carrier ... see `kDrums`.
	if (impl->variant != Variant::opll && impl->variant != Variant::opl3
		&& channel >= impl->rhythmBase && channel < impl->rhythmBase + 5)
	{
		// Each drum's slots, as `regbase2modop` numbers them where the chip reads them:
		// `op[6]`/`op[15]` are ch6's modulator and carrier (offset 16 and 19 -> `0x30`/`0x33`),
		// `op[7]`/`op[16]` ch7's (`0x31`/`0x34`), `op[8]`/`op[17]` ch8's (`0x32`/`0x35`).
		struct DrumSlots
		{
			uint8_t slots[2];
			int count;
		};

		static constexpr DrumSlots kDrums[5] = {
			{ { 6, 15 }, 2 },		// BD: ch6 modulator and carrier
			{ { 16, 7 }, 2 },		// SD: ch7 carrier, and the modulator that shares it
			{ { 8, 17 }, 2 },		// TOM: ch8 modulator and carrier
			{ { 17, 0 }, 1 },		// CYM: ch8 carrier
			{ { 7, 0 }, 1 },		// HH: ch7 modulator
		};

		const int drum = channel - impl->rhythmBase;
		const Impl::Channel& owner = impl->channels[6 + (drum == 0 ? 0 : (drum == 1 ? 1 : 2))];

		out = VoicePatch {};
		out.model = ChipModel::ym3812;
		out.oneShot = true;
		out.triggerMask = (uint8_t) (0x10u >> drum);
		out.lengthSeconds = 0.30;

		// **Measured against a melodic note**, the same way the OPLL's `0.5` was: one hit at trim 1.0
		// peaks at `0.25` on a sixteen-bit file, so a hit and a note sit at the same level.
		out.trim = 1.0f;

		// **The core's operator index is not the register offset.** `regbase2modop`
		// (`adlibemu_opl_inc.c:89`) is the table: ops 6, 7, 8 are the modulators of channels 6-8 and
		// are addressed at offsets 16, 17, 18; ops 15, 16, 17 are their carriers, at 19, 20, 21.
		static constexpr int kOffsetOfOp[18] = { -1, -1, -1, -1, -1, -1, 16, 17, 18,
												 -1, -1, -1, -1, -1, -1, 19, 20, 21 };
		static constexpr int kOperatorOfOp[18] = { 0, 0, 0, 0, 0, 0, 0, 0, 0,
												  0, 0, 0, 0, 0, 0, 1, 1, 1 };

		for (int i = 0; i < kDrums[drum].count; ++i)
		{
			const int op = kDrums[drum].slots[i];

			if (op < 0 || op > 17 || kOffsetOfOp[op] < 0)
			{
				continue;
			}

			const int offset = kOffsetOfOp[op];
			const Impl::Channel& voice = impl->channels[6 + ((offset - 16) % 3)];

			out.regs.push_back ({ (uint16_t) (0x20 + offset), voice.opRegs[kOperatorOfOp[op]][0] });
			out.regs.push_back ({ (uint16_t) (0x40 + offset), voice.opRegs[kOperatorOfOp[op]][1] });
			out.regs.push_back ({ (uint16_t) (0x60 + offset), voice.opRegs[kOperatorOfOp[op]][2] });
			out.regs.push_back ({ (uint16_t) (0x80 + offset), voice.opRegs[kOperatorOfOp[op]][3] });
		}

		// The two drums with a pitch, from the writes that used to be discarded.
		if (drum == 0 || drum == 2)
		{
			const int ch = (drum == 0) ? 6 : 8;

			out.regs.push_back ({ (uint16_t) (0xA0 + ch), (uint16_t) (owner.fnum & 0xFF) });
			out.regs.push_back ({ (uint16_t) (0xB0 + ch),
								  (uint16_t) (((owner.block & 0x07) << 2) | ((owner.fnum >> 8) & 0x03)) });
		}

		return true;
	}

	// The OPL3 needs a second case - its second bank and its `0xE0` waveform registers.
	if (channel < 0 || channel >= melodic || impl->variant == Variant::opl3)
	{
		return false;
	}

	const Impl::Channel& voice = impl->channels[channel];

	out = VoicePatch {};
	out.model = impl->patchModel ();

	if (impl->variant != Variant::opll)
	{
		// **The chip's own nine registers, for channel 0.** Nine rather than eight because the
		// connection bit is what makes a two-operator voice additive or serial, and that changes the
		// shape more than any single level does.
		//
		// **Channel 0, not the song's channel**: the slot formula is
		// `slot = (ch/3)*6 + ch%3 + 3*op`, so recovering the channel from a register means walking
		// `kOperatorOfRegister` backwards. A patch is rendered on a core of its own and only ever
		// portrays one voice, which is what makes the fixed addresses correct rather than lossy.
		static constexpr uint16_t kBankBase[4] = { 0x20, 0x40, 0x60, 0x80 };
		static constexpr uint16_t kSlotOf[2] = { 0, 3 };		// modulator, carrier, for channel 0

		for (int op = 0; op < 2; ++op)
		{
			for (int bank = 0; bank < 4; ++bank)
			{
				out.regs.push_back ({ (uint16_t) (kBankBase[bank] + kSlotOf[op]),
									  voice.opRegs[op][bank] });
			}
		}

		out.regs.push_back ({ (uint16_t) 0xC0, voice.feedbackConnection });
		return true;
	}

	// **The user voice first, and it has to be first.** Instrument 0 means "the eight global
	// registers", and the core copies that patch into the channel at the moment `0x30` is written -
	// so writing the registers afterwards would leave the channel holding the previous ones.
	if (voice.instrument == 0)
	{
		for (int i = 0; i < 8; ++i)
		{
			out.regs.push_back ({ (uint16_t) i, impl->userPatch[i] });
		}
	}

	// The selector, for channel 0: the high nibble picks the ROM instrument (or 0 for the eight bytes
	// above) and the low nibble is this channel's volume **as the chip takes it** - an attenuation
	// where zero is loudest. Copied rather than inverted, because the patch is the register, and a
	// quiet channel should export quiet.
	out.regs.push_back ({ (uint16_t) 0x30, (uint16_t) ((voice.instrument << 4) | voice.volume) });

	return true;
}

} // namespace toybox::chips
