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

/** **The core that renders a model.** Not a property of the patch - the patch only names the chip -
	so it lives with the other translation from a model to an emulator concept, and `fmOutputBits`
	beside it in `chips/VoicePatch.h` states how wide each of these actually accumulates.

	`unknown` falls back to the OPN: it is the chip this project has the most of, and a renderer that
	refuses to draw anything is worse than one that draws the right shape at the wrong width.
*/
constexpr DEV_ID deviceIdFor (chips::ChipModel model)
{
	switch (model)
	{
	case chips::ChipModel::ym2608:		return DEVID_YM2608;
	case chips::ChipModel::ym2610:		return DEVID_YM2610;
	case chips::ChipModel::ym2151:		return DEVID_YM2151;
	case chips::ChipModel::ym3812:		return DEVID_YM3812;

	case chips::ChipModel::ym2612:
	case chips::ChipModel::vrc7:
	case chips::ChipModel::unknown:	break;
	}

	return DEVID_YM2612;
}

/** **Full scale for a `DEV_SMPL` straight from a chip core.**

	Sixteen bits, not thirty-two: `VGMPlayer` shifts its output left by eight before handing it on,
	which is where the playback path's `2^31` comes from. Reading `devDef->Update` directly gives
	the core's own numbers, and the oracle test's silence floor of `1.0` is that same scale.
*/
constexpr double kSamplesToFloat = 1.0 / 32768.0;

// The two stage lengths live in chips/VoicePatch.h, where the exporter can reach them too.

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

	explicit Core (double rate, chips::ChipModel model)
	{
		DEV_GEN_CFG config {};
		config.emuCore = 0;					// the default core: the one the plugin plays
		config.srMode = DEVRI_SRMODE_CUSTOM;
		config.flags = 0;
		config.clock = kClock;
		config.smplRate = (uint32_t) std::lround (rate);

		ready = SndEmu_Start (deviceIdFor (model), &config, &info) == EERR_OK;

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

	// The fallbacks, in the same place `render()` finds them: two stages, held and released.
	return chips::kDefaultSustainSeconds + chips::kDefaultReleaseSeconds;
}

std::vector<float> render (const VoicePatch& patch, double frequencyHz,
						   double sustainSeconds, double releaseSeconds, double rate,
						   bool normalise)
{
	if (patch.regs.empty () || frequencyHz <= 0.0)
	{
		return {};
	}

	// **The chip's own rate unless the caller names one.** Rendering at 8 kHz and holding the samples
	// up to the export rate is what made a dragged file look like it came from an eight-bit machine:
	// five and a half samples of one level, then a step. At the chip's rate the steps are the
	// converter's, which are the ones worth seeing.
	const double renderRate = (rate > 0.0) ? rate : chips::noteRateFor (patch.model);

	Core core (renderRate, patch.model);
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

	// **Two stages, both given, neither read from the patch.** The registers are honest and
	// unusable: a release code of 1 is a sweep of four minutes on this chip, and the sustain is a
	// level the envelope decays *through* rather than holds at, so it has no duration to read at
	// all. How long to hold and how long a tail to keep are choices - the caller brings them, and
	// the defaults stand in when it brings nothing.
	if (sustainSeconds <= 0.0)
	{
		sustainSeconds = chips::kDefaultSustainSeconds;
	}

	if (releaseSeconds <= 0.0)
	{
		releaseSeconds = chips::kDefaultReleaseSeconds;
	}

	const double total = sustainSeconds + releaseSeconds;
	const int cap = (int) std::lround (std::max (0.05, total) * renderRate);

	// Key off exactly where the sustain ends, so the tail is the release the caller asked for.
	const int releaseAt = std::min (cap - 1, (int) std::lround (sustainSeconds * renderRate));

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
	if (normalise)
	{
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
	}

	// **Quantised to the core's own resolution, not to a number picked by eye.**
	//
	// `fmopn` sums four operators into `out_fm`, each about `+/-0x1000`, so its samples carry
	// fourteen bits of magnitude - that is what `DEV_SMPL` from this core means, and dividing by
	// 32768 keeps every bit of it. An earlier version quantised to eight, on the reasoning that an
	// eight-bit *converter* is the chip's signature; but the DAC's width is not the core's, and
	// throwing away six bits of detail the hardware actually produced is a different sound, not a
	// more authentic one.
	//
	// The step is `2^(bits-1) - 1` for whatever this patch's chip accumulates - fourteen for the
	// OPN family, sixteen for OPL and OPM - and it is applied last, so full scale stays full scale.
	const int bits = chips::fmOutputBits (patch.model);
	const float quantiseSteps = (float) ((1 << (std::clamp (bits, 2, 16) - 1)) - 1);

	for (auto& sample : out)
	{
		sample = std::round (sample * quantiseSteps) / quantiseSteps;
	}

	return out;
}

} // namespace toybox::chips::noteWave
