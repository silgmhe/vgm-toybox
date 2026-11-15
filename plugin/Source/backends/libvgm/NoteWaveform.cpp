#include "NoteWaveform.h"

#include <algorithm>
#include <cmath>

#include "emu/EmuStructs.h"
#include "emu/SoundDevs.h"
#include "emu/SoundEmu.h"

namespace toybox::chips::noteWave
{
namespace
{

/** The YM2612's clock, the Mega Drive's - the same one the decoder's note table assumes. */
constexpr uint32_t kClock = 7670453;

/** **Full scale for a `DEV_SMPL` straight from a chip core.**

	Sixteen bits, not thirty-two: `VGMPlayer` shifts its output left by eight before handing it on,
	which is where the playback path's `2^31` comes from. Reading `devDef->Update` directly gives
	the core's own numbers, and the oracle test's silence floor of `1.0` is that same scale.
*/
constexpr double kSamplesToFloat = 1.0 / 32768.0;

/** How long a note is held before key-off. The release after it gets the rest of the buffer. */
constexpr double kHoldSeconds = 0.6;

/** One YM2612, alive for the length of one render.

	**The same core the plugin plays through** - `emuCore = 0` is the default one, which for this
	project is `fmopn.c`. That is the whole point of rendering this way: what a panel draws and
	what a listener hears come from the same code, so they cannot disagree about a patch.
*/
struct Core
{
	DEV_INFO info {};
	DEVFUNC_WRITE_A8D8 writeFn = nullptr;
	bool ready = false;

	explicit Core (double rate)
	{
		DEV_GEN_CFG config {};
		config.emuCore = 0;					// the default core: the one the plugin plays
		config.srMode = DEVRI_SRMODE_CUSTOM;
		config.flags = 0;
		config.clock = kClock;
		config.smplRate = (uint32_t) std::lround (rate);

		ready = SndEmu_Start (DEVID_YM2612, &config, &info) == EERR_OK;

		if (ready)
		{
			ready = SndEmu_GetDeviceFunc (info.devDef, RWF_REGISTER | RWF_WRITE, DEVRW_A8D8, 0,
										   (void**) &writeFn) == EERR_OK;
		}
	}

	~Core ()
	{
		if (ready)
		{
			SndEmu_Stop (&info);
		}

		SndEmu_FreeDevLinkData (&info);
	}

	Core (const Core&) = delete;
	Core& operator= (const Core&) = delete;

	/** An address byte then a data byte, which is how an A8D8 device is written. The port sits in
		the address's bit 8, so `port * 2` selects either register latch. */
	void write (int port, uint8_t reg, uint8_t value)
	{
		if (writeFn == nullptr)
		{
			return;
		}

		writeFn (info.dataPtr, (uint8_t) (port * 2), reg);
		writeFn (info.dataPtr, (uint8_t) (port * 2 + 1), value);
	}
};

} // namespace

double durationOf (const VoicePatch& patch)
{
	(void) patch;

	// An upper bound rather than a prediction. The chip decides how long a note lasts, and
	// `render()` stops when it goes quiet - this only sizes the buffer it may fill.
	return 1.5;
}

std::vector<float> render (const VoicePatch& patch, double frequencyHz,
						   double seconds, double rate)
{
	if (patch.regs.empty () || frequencyHz <= 0.0)
	{
		return {};
	}

	Core core (rate);
	if (! core.ready)
	{
		return {};
	}

	// The channel is the low two bits of the addresses the patch came back with, and the port is in
	// bit 8 - see VoicePatch.h for why both are needed.
	const uint16_t first = patch.regs.front ().first;
	const int channelInPort = (int) (first & 0x03);
	const int port = (first & 0x100) ? 1 : 0;
	const int chipChannel = port * 3 + channelInPort;

	// **The three registers a bare core needs before it will make a sound.** A freshly started
	// YM2612 has the LFO and the timers in whatever state the core's constructor leaves them, and
	// channel 3 may be in special mode; the oracle test writes exactly these before it plays
	// anything. Without them every note came out silent, which is what "all zero samples" looks
	// like from the outside.
	core.write (0, 0x22, 0x00);		// LFO off: a tremolo would make the level time-dependent
	core.write (0, 0x27, 0x00);		// no timers, channel 3 in normal mode
	core.write (0, 0x2B, 0x00);		// DAC off: this is a voice, not the sample channel

	for (const auto& reg : patch.regs)
	{
		core.write (port, (uint8_t) (reg.first & 0xFF), (uint8_t) reg.second);
	}

	// **The pitch.** The channel state carries a frequency and the chip wants an f-number and a
	// block; the block is chosen so the f-number lands inside its eleven bits, which is what a
	// driver does with a note in any octave.
	int block = 4;
	double fnum = frequencyHz * 144.0 * std::exp2 (20.0 - (double) block) / (double) kClock;

	while (fnum >= 2048.0 && block < 7)
	{
		++block;
		fnum = frequencyHz * 144.0 * std::exp2 (20.0 - (double) block) / (double) kClock;
	}

	while (fnum < 1.0 && block > 0)
	{
		--block;
		fnum = frequencyHz * 144.0 * std::exp2 (20.0 - (double) block) / (double) kClock;
	}

	const auto fnumBits = (uint16_t) std::lround (std::clamp (fnum, 0.0, 2047.0));

	core.write (port, (uint8_t) (0xA0 + channelInPort), (uint8_t) (fnumBits & 0xFF));
	core.write (port, (uint8_t) (0xA4 + channelInPort),
				(uint8_t) (((fnumBits >> 8) & 0x07) | (block << 3)));

	// Key on all four operators. The key register lives on the first port and carries the
	// chip-wide channel number in its low three bits.
	core.write (0, 0x28, (uint8_t) (0xF0 | chipChannel));

	const int cap = (int) std::lround (std::max (0.05, seconds) * rate);
	const int releaseAt = std::min (cap - 1, (int) std::lround (kHoldSeconds * rate));

	std::vector<float> out ((size_t) cap, 0.0f);
	std::vector<DEV_SMPL> left (512, 0);
	std::vector<DEV_SMPL> right (512, 0);

	int done = 0;
	int quietRun = 0;
	bool released = false;

	while (done < cap)
	{
		if (! released && done >= releaseAt)
		{
			core.write (0, 0x28, (uint8_t) chipChannel);
			released = true;
		}

		const int count = std::min (512, cap - done);
		DEV_SMPL* outputs[2] = { left.data (), right.data () };
		core.info.devDef->Update (core.info.dataPtr, (uint32_t) count, outputs);

		for (int i = 0; i < count; ++i)
		{
			const double value = ((double) left[(size_t) i] + (double) right[(size_t) i]) * 0.5;
			const auto sample = (float) (value * kSamplesToFloat);
			out[(size_t) (done + i)] = sample;

			quietRun = (std::abs (sample) < 1.0e-4f) ? quietRun + 1 : 0;
		}

		done += count;

		// **Stop when the note has ended**, rather than padding every note to the buffer's length:
		// that is what makes a blip short and a pad long without predicting either.
		if (released && quietRun > (int) (rate * 0.02))
		{
			out.resize ((size_t) done);
			break;
		}
	}

	return out;
}

} // namespace toybox::chips::noteWave
