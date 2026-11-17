#include "Vrc7VoiceDriver.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "chips/VoicePatch.h"

#include "xgm/devices/Sound/legacy/emu2413.h"

namespace toybox::backends::nsf
{
namespace
{

/** The OPLL divides its clock by 72 to get the rate its pitch table is expressed in. */
constexpr double kDivider = 72.0;

/** `OPLL_calcStereo` writes a pair of `int32_t` in the chip's own scale, which is sixteen bits. */
constexpr double kSamplesToFloat = 1.0 / 32768.0;

/** Six voices on the VRC7; the OPLL addresses nine, but a cartridge wires up six. */
constexpr int kVoices = 6;

/** The register banks that carry a voice's instrument and volume. The patch data itself is in the
	chip's ROM table, addressed by the instrument number in the high nibble - which is why a VRC7
	patch cannot be carried by registers alone, and why the driver loads the same table the player
	does. */
constexpr uint8_t kInstrumentBase = 0x30;

class Vrc7VoiceDriver final : public playback::IVoiceDriver
{
public:
	explicit Vrc7VoiceDriver (double clockHz)
		: chipClock (clockHz > 0.0 ? clockHz : 3579545.0),
		  sampleRate (chipClock / kDivider)
	{
	}

	~Vrc7VoiceDriver () override
	{
		if (opll != nullptr)
		{
			OPLL_delete (opll);
		}
	}

	Vrc7VoiceDriver (const Vrc7VoiceDriver&) = delete;
	Vrc7VoiceDriver& operator= (const Vrc7VoiceDriver&) = delete;

	bool load (const chips::VoicePatch& patch) override
	{
		if (patch.model != chips::ChipModel::vrc7 || patch.regs.empty ())
		{
			return false;
		}

		if (! ensureStarted ())
		{
			return false;
		}

		// The channel is whichever banked register the patch carries. Written before the others so a
		// patch that also carries a key or a pitch lands on the right voice.
		for (const auto& reg : patch.regs)
		{
			const auto address = (uint8_t) (reg.first & 0xFF);

			if (address >= 0x10 && address < (uint8_t) (0x10 + kVoices))
			{
				channel = address - 0x10;
				break;
			}
		}

		// **Only the instrument and volume.** A VRC7 voice's attack, decay and release are not in
		// these registers - the cartridge selects one of sixteen patches burned into the chip - so
		// writing anything else here would be writing a value the chip does not read.
		for (const auto& reg : patch.regs)
		{
			const auto address = (uint8_t) (reg.first & 0xFF);

			if (address >= kInstrumentBase && address < (uint8_t) (kInstrumentBase + kVoices))
			{
				write (address, (uint8_t) reg.second);
			}
		}

		return true;
	}

	void noteOn (double hz) override
	{
		if (opll == nullptr || hz <= 0.0)
		{
			return;
		}

		// **f = fnum * clock / (72 * 2^(19 - block))**, inverted. The block is chosen so the
		// f-number lands inside its nine bits, which is what a driver does with a note in any octave.
		int block = 4;
		double fnum = hz * kDivider * std::exp2 (19.0 - (double) block) / chipClock;

		while (fnum >= 512.0 && block < 7)
		{
			++block;
			fnum = hz * kDivider * std::exp2 (19.0 - (double) block) / chipClock;
		}

		while (fnum < 1.0 && block > 0)
		{
			--block;
			fnum = hz * kDivider * std::exp2 (19.0 - (double) block) / chipClock;
		}

		const auto fnumBits = (int) std::lround (std::clamp (fnum, 0.0, 511.0));

		write ((uint8_t) (0x10 + channel), (uint8_t) (fnumBits & 0xFF));

		// 0x20 carries the f-number's top bit, the block, and the key. Bit 5 is the sustain switch,
		// which a cartridge-driven voice leaves on so the note holds at its sustain level.
		const auto high = (uint8_t) (((fnumBits >> 8) & 0x01) | (block << 1) | 0x10);
		write ((uint8_t) (0x20 + channel), high);
	}

	void noteOff (void) override
	{
		if (opll == nullptr)
		{
			return;
		}

		// Clearing bit 4 releases the note; the rest of the byte has to survive, so it is re-read
		// from the chip rather than reconstructed from what `noteOn` last sent.
		write ((uint8_t) (0x20 + channel), (uint8_t) (opll->reg[0x20 + channel] & ~0x10));
	}

	void render (float* out, int frames) override
	{
		if (opll == nullptr || out == nullptr || frames <= 0)
		{
			return;
		}

		for (int i = 0; i < frames; ++i)
		{
			int32_t stereo[2] = { 0, 0 };
			OPLL_calcStereo (opll, stereo);

			const double value = ((double) stereo[0] + (double) stereo[1]) * 0.5;
			out[i] = (float) (value * kSamplesToFloat);
		}
	}

	double rate (void) const override { return sampleRate; }

private:
	bool ensureStarted (void)
	{
		if (opll != nullptr)
		{
			return true;
		}

		opll = OPLL_new ((uint32_t) std::lround (chipClock), (uint32_t) std::lround (sampleRate));

		if (opll == nullptr)
		{
			return false;
		}

		// **The same patch table the player uses.** A VRC7 voice is one of sixteen patches in the
		// chip's ROM; picking the wrong table would render a correct instrument number as somebody
		// else's instrument.
		OPLL_resetPatch (opll, OPLL_VRC7_TONE);

		return true;
	}

	/** An address, then the byte for it - the interface the cartridge itself has. */
	void write (uint8_t address, uint8_t value)
	{
		OPLL_writeIO (opll, 0, address);
		OPLL_writeIO (opll, 1, value);
	}

	OPLL* opll = nullptr;
	double chipClock = 0.0;
	double sampleRate = 0.0;
	int channel = 0;
};

} // namespace

std::unique_ptr<playback::IVoiceDriver> makeVrc7VoiceDriver (double clockHz)
{
	return std::make_unique<Vrc7VoiceDriver> (clockHz);
}

} // namespace toybox::backends::nsf
