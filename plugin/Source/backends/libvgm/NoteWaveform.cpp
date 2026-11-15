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

/** How long a note is held before key-off. The release after it gets the rest of the buffer -
	with a four-second buffer this leaves a second and a half for the tail, which is long enough to
	see whether the patch releases slowly or snaps. */
constexpr double kHoldSeconds = 2.5;

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

	// **Four seconds, regardless of the patch.** Guessing a length from the envelope was worse than
	// useless: a driver-built timbre has `DR` at its fastest and does its decay by rewriting `TL`
	// every frame, so the registers alone say "over in 30 ms" while the note lasts a second. Four
	// seconds always contains one note, and a note that ends sooner simply leaves the rest quiet.
	return 4.0;
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

	// **0xA4 before 0xA0, and the order is load-bearing.** The chip latches the whole f-number when
	// 0xA0 is written: the low byte from that write and the high bits and block from whatever 0xA4
	// last held. Writing 0xA0 first therefore locks in high = 0 and block = 0 - a frequency of a
	// few hertz - and the 0xA4 that follows arrives too late to be counted. The result is a slow
	// swell with no carrier, which is exactly what the first version of this drew.
	core.write (port, (uint8_t) (0xA4 + channelInPort),
				(uint8_t) (((fnumBits >> 8) & 0x07) | (block << 3)));
	core.write (port, (uint8_t) (0xA0 + channelInPort), (uint8_t) (fnumBits & 0xFF));

	// **Key on the way a driver does it**, which is three writes and not one:
	//
	//  0x28 = 0x00 | channelKey    first, so the envelope starts from silence. A key-on over a
	//                              voice that is still sounding resumes it at its current level,
	//                              which for a freshly written patch is whatever the core's default
	//                              was - often nothing.
	//  0x28 = 0xF0 | channelKey    then, the actual key.
	//  0xB4 = 0xC0                 and the pan, both sides. **The core starts with it closed**, so
	//                              every note rendered without this line came out at zero.
	//
	// `channelKey` is `(bank << 2) | slot`, not a channel number from 0 to 5; the flat number keyed
	// a neighbour for everything past the first bank. FamiStudio's EPSM channel state does exactly
	// these three writes on a note trigger.
	const auto channelKey = (uint8_t) ((port << 2) | channelInPort);

	core.write (0, 0x28, channelKey);
	core.write (0, 0x28, (uint8_t) (0xF0 | channelKey));
	core.write (port, (uint8_t) (0xB4 + channelInPort), 0xC0);

	// **A caller that passes zero asks for `durationOf()`.** This paragraph went missing in an edit
	// and the clamp below turned the zero into its 50 ms floor - which is precisely the length every
	// exported note came out at, and why `durationOf`'s four seconds were never reached.
	if (seconds <= 0.0)
	{
		seconds = durationOf (patch);
	}

	const int cap = (int) std::lround (std::max (0.05, seconds) * rate);
	const int releaseAt = std::min (cap - 1, (int) std::lround (kHoldSeconds * rate));

	std::vector<float> out ((size_t) cap, 0.0f);
	std::vector<DEV_SMPL> left (512, 0);
	std::vector<DEV_SMPL> right (512, 0);

	int done = 0;
	bool released = false;

	while (done < cap)
	{
		if (! released && done >= releaseAt)
		{
			core.write (0, 0x28, channelKey);
			released = true;
		}

		const int count = std::min (512, cap - done);
		DEV_SMPL* outputs[2] = { left.data (), right.data () };
		core.info.devDef->Update (core.info.dataPtr, (uint32_t) count, outputs);

		for (int i = 0; i < count; ++i)
		{
			const double value = ((double) left[(size_t) i] + (double) right[(size_t) i]) * 0.5;
			out[(size_t) (done + i)] = (float) (value * kSamplesToFloat);
		}

		done += count;
	}

	// **Normalised to its own peak.** The absolute level a patch produces is meaningless outside the
	// song: a carrier at total level 48 is -36 dB by design, and a modulator at 0x7F is inaudible.
	// Drawn at that level the window shows a flat line, and the exported file is a whisper - both
	// correct and both useless. The shape is what is being shown, so the shape is what fills the
	// range; the relative levels between the four operators, which is what makes the timbre, are
	// untouched by a single gain.
	float peak = 0.0f;
	for (float sample : out)
	{
		peak = std::max (peak, std::abs (sample));
	}

	if (peak > 1.0e-6f)
	{
		const float gain = 1.0f / peak;

		for (auto& sample : out)
		{
			sample *= gain;
		}
	}

	return out;
}

} // namespace toybox::chips::noteWave
