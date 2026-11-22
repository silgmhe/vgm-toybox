#include "OpnVoiceDriver.h"

#include <algorithm>
#include <cmath>

#include "emu/EmuStructs.h"
#include "emu/SoundDevs.h"
#include "emu/SoundEmu.h"

namespace toybox::backends::libvgm
{
namespace
{

/** The YM2612's clock, the Mega Drive's - the same one the decoder's note table assumes. */
constexpr uint32_t kClock = 7670453;

/** How many samples to pull from the core at a time. */
constexpr int kBlock = 512;

/** **Full scale for a `DEV_SMPL` straight from a chip core.**

	Sixteen bits, not thirty-two: `VGMPlayer` shifts its output left by eight before handing it on,
	which is where the playback path's `2^31` comes from. Reading `devDef->Update` directly gives the
	core's own numbers, and the oracle test's silence floor of `1.0` is that same scale.
*/
constexpr double kSamplesToFloat = 1.0 / 32768.0;

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
	case chips::ChipModel::ym2203:		return DEVID_YM2203;
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

/** **The clock each model is normally run at**, so that this agrees with `noteRateFor`.

	`noteRateFor` already divides by each chip's own divider - 144 for the OPN family, 64 for the
	OPM, 72 for the OPL - while the core used to be started at the Mega Drive's clock whatever the
	model named.

	**That mismatch does not show in the pitch.** The clock scales both the f-number and the core's
	own rate, so the two cancel: giving every model 7 670 453 Hz and re-running the pitch tests
	leaves both of them passing. What it does reach is the things a core derives from its clock and
	nothing else - the ADPCM playback rates on the OPNA and OPNB, the timers, the YM2612's DAC -
	**and none of those has an assertion yet**.

	So this corrects an inconsistency rather than a reported symptom. It is kept because a chip
	claiming another chip's clock is the kind of thing that surfaces later as a sample playing at the
	wrong speed, and because `noteRateFor` naming one number while this named another was already two
	answers to one question.

	VRC7 is absent on purpose. It is not a libvgm device - the NSF side hands its own clock to its
	own OPLL - so the fallback below is only ever the OPN2's.
*/
constexpr uint32_t clockFor (chips::ChipModel model)
{
	switch (model)
	{
	case chips::ChipModel::ym2203:	return 3993600;
	case chips::ChipModel::ym2608:
	case chips::ChipModel::ym2610:	return 8000000;
	case chips::ChipModel::ym2151:	return 3579545;
	case chips::ChipModel::ym3812:	return 3579545;

	case chips::ChipModel::ym2612:
	case chips::ChipModel::vrc7:
	case chips::ChipModel::unknown:	break;
	}

	return kClock;
}

/** **Which parts have to be told they are six-channel, and how.**

	`0x29` bit 7 is the "extend 3ch" switch. `reset` leaves it clear, so an OPNA or OPNB comes back
	as a three-channel part - the reason channels 4-6 held a complete patch that nothing ever started
	(see `load`).

	**A YM2612 has six without being told** and **a YM2203 has three**, so neither is written to: on
	the first the register means nothing, on the second setting the bit would open channels that do
	not exist.

	A named function rather than an `if` in `load`, because the same set of variants decides the
	divisor in `OpnDecoder::addressShiftFor`. Adding a part means editing both, and this at least
	makes the second edit a search away rather than a guess.
*/
constexpr bool hasSixChannelMode (chips::ChipModel model)
{
	switch (model)
	{
	case chips::ChipModel::ym2608:
	case chips::ChipModel::ym2610:
		return true;

	default:
		return false;
	}
}

/** Bit 7 sets six channels; the low five are the reset value of the IRQ mask, written back
	unchanged so that nothing masked becomes enabled.
*/
constexpr uint8_t sixChannelMode = 0x80 | 0x1F;

} // namespace

/** One core, alive across notes.

	**The same core the plugin plays through** - `emuCore = 0` is the default one, which for this
	project is `fmopn.c`. That is the whole point of rendering this way: what a panel draws and what
	a listener hears come from the same code, so they cannot disagree about a patch.
*/
struct OpnVoiceDriver::Core
{
	DEV_INFO info {};
	DEVFUNC_WRITE_A8D8 writeFn = nullptr;
	double rate = 0.0;
	bool ready = false;

	Core (double sampleRate, chips::ChipModel model)
		: rate (sampleRate)
	{
		DEV_GEN_CFG config {};
		config.emuCore = 0;					// the default core: the one the plugin plays
		config.srMode = DEVRI_SRMODE_CUSTOM;
		config.flags = 0;
		config.clock = clockFor (model);
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

	/** **Everything back to silence.**

		One core serves every channel of a song, so the channel rendered before this one is still
		keyed on - or still releasing - when the next patch arrives. Writing a new patch over it does
		not stop it: the release keeps sounding, and its notes end up inside the next channel's
		waveform. That is what "shared registers" looks like from the outside: two rows drawing the
		same shape, or one drawing a mix of two timbres.

		`Reset` rather than `SndEmu_Stop` + `SndEmu_Start`: it is the same call `SndEmu_Start` makes
		once the core exists, and it costs a state clear rather than an allocation.
	*/
	void reset (void)
	{
		if (ready && info.devDef != nullptr && info.devDef->Reset != nullptr)
		{
			info.devDef->Reset (info.dataPtr);
		}
	}

	/** An address byte then a data byte, which is how an A8D8 device is written. The port sits in
		the address's bit 8, so `port * 2` selects either register latch. */
	void write (int writePort, uint8_t reg, uint8_t value)
	{
		if (writeFn == nullptr)
		{
			return;
		}

		writeFn (info.dataPtr, (uint8_t) (writePort * 2), reg);
		writeFn (info.dataPtr, (uint8_t) (writePort * 2 + 1), value);
	}
};

OpnVoiceDriver::~OpnVoiceDriver () = default;

std::unique_ptr<playback::IVoiceDriver> makeOpnVoiceDriver (void)
{
	return std::make_unique<OpnVoiceDriver> ();
}

bool OpnVoiceDriver::startCore (chips::ChipModel model)
{
	core = std::make_unique<Core> (chips::noteRateFor (model), model);
	loadedModel = model;

	return core != nullptr && core->ready;
}

bool OpnVoiceDriver::load (const chips::VoicePatch& patch)
{
	if (patch.regs.empty ())
	{
		return false;
	}

	// Rebuild only when the chip changes. A driver serving six channels of one YM2612 keeps one
	// core; a backend with an OPN and an OPM rebuilds when the patch names a different chip.
	if (core == nullptr || ! core->ready || loadedModel != patch.model)
	{
		if (! startCore (patch.model))
		{
			return false;
		}
	}

	// The channel is the low two bits of the addresses the patch came back with, and the port is in
	// bit 8 - see VoicePatch.h for why both are needed.
	const uint16_t first = patch.regs.front ().first;
	channelInPort = (int) (first & 0x03);
	port = (first & 0x100) ? 1 : 0;

	// **Start from silence, not from whatever the last channel left.** The core is shared, and the
	// previous channel is still releasing when this patch arrives - see `reset`.
	core->reset ();

	// **The three registers a bare core needs before it will make a sound.** A freshly started
	// YM2612 has the LFO and the timers in whatever state the core's constructor leaves them, and
	// channel 3 may be in special mode. Without them every note came out silent, which is what "all
	// zero samples" looks like from the outside.
	core->write (0, 0x22, 0x00);		// LFO off: a tremolo would make the level time-dependent
	core->write (0, 0x27, 0x00);		// no timers, channel 3 in normal mode
	core->write (0, 0x2B, 0x00);		// DAC off: this is a voice, not the sample channel

	// **The OPNA and OPNB have to be told they are six-channel parts, and `reset` un-tells them.**
	// 0x29 bit 7 is the "extend 3ch" switch: the core's own reset writes 0x29 = 0x1F, whose bit 7
	// is clear, so the chip comes back as a three-channel OPN. The register writes still land in
	// the right place - 0x30|0x100 reaches channel 4 because that path tests the address, not the
	// mode - but 0x28 does not: its bank bit is ignored while the mode is off, so `0xF4` keys
	// channel 1 instead of channel 4. Channels 4-6 then hold a patch nothing ever starts, and the
	// three rows above sound as if only half the chip worked.
	if (hasSixChannelMode (patch.model))
	{
		core->write (0, 0x29, sixChannelMode);	// six channels, IRQ mask at the reset default
	}

	for (const auto& reg : patch.regs)
	{
		core->write (port, (uint8_t) (reg.first & 0xFF), (uint8_t) reg.second);
	}

	return true;
}

void OpnVoiceDriver::noteOn (double hz)
{
	if (core == nullptr || ! core->ready || hz <= 0.0)
	{
		return;
	}

	// **The pitch.** The chip wants an f-number and a block; the block is chosen so the f-number
	// lands inside its eleven bits, which is what a driver does with a note in any octave.
	// **This chip's clock, not the Mega Drive's.** It cancels out of the pitch - the f-number and the
	// core's own rate both scale with it - so this is not where a flat note would come from; the
	// divisor below is. What it does carry is the rates and timers a core derives from its clock.
	// See `clockFor` for what is measured and what is not.
	const double clock = (double) clockFor (loadedModel);

	// **144 for the OPN2, 288 for the OPNA/OPNB**, which is where the doubling hides. libvgm's
	// OPNA core derives its frequency step from clock / 288 - the master clock halved before the
	// prescaler - while `noteRateFor` reports clock / 144 as its output rate, because that core
	// runs at twice the fundamental. Writing 144 here made every OPNA note an octave flat.
	const double divisor = (loadedModel == chips::ChipModel::ym2608
							|| loadedModel == chips::ChipModel::ym2610) ? 288.0 : 144.0;

	int block = 4;
	double fnum = hz * divisor * std::exp2 (20.0 - (double) block) / clock;

	while (fnum >= 2048.0 && block < 7)
	{
		++block;
		fnum = hz * divisor * std::exp2 (20.0 - (double) block) / clock;
	}

	while (fnum < 1.0 && block > 0)
	{
		--block;
		fnum = hz * divisor * std::exp2 (20.0 - (double) block) / clock;
	}

	const auto fnumBits = (uint16_t) std::lround (std::clamp (fnum, 0.0, 2047.0));

	// **0xA4 before 0xA0, and the order is load-bearing.** The chip latches the whole f-number when
	// 0xA0 is written: the low byte from that write, the high bits and block from whatever 0xA4 last
	// held. Writing 0xA0 first locks in high = 0 and block = 0 - a few hertz - and the 0xA4 that
	// follows arrives too late to be counted. The result is a slow swell with no carrier, which is
	// exactly what the first version of this drew.
	core->write (port, (uint8_t) (0xA4 + channelInPort),
				 (uint8_t) (((fnumBits >> 8) & 0x07) | (block << 3)));
	core->write (port, (uint8_t) (0xA0 + channelInPort), (uint8_t) (fnumBits & 0xFF));

	// **Key on the way a driver does it**, which is three writes and not one:
	//
	//  0x28 = 0x00 | channelKey    first, so the envelope starts from silence. A key-on over a voice
	//                              that is still sounding resumes it at its current level, which for
	//                              a freshly written patch is whatever the core's default was.
	//  0x28 = 0xF0 | channelKey    then the actual key.
	//  0xB4 = 0xC0                 and the pan, both sides. **The core starts with it closed**, so
	//                              every note rendered without this line came out at zero.
	//
	// `channelKey` is `(bank << 2) | slot`, not a channel number from 0 to 5; the flat number keyed
	// a neighbour for everything past the first bank.
	const auto channelKey = (uint8_t) ((port << 2) | channelInPort);

	core->write (0, 0x28, channelKey);
	core->write (0, 0x28, (uint8_t) (0xF0 | channelKey));
	core->write (port, (uint8_t) (0xB4 + channelInPort), 0xC0);
}

void OpnVoiceDriver::noteOff (void)
{
	if (core == nullptr || ! core->ready)
	{
		return;
	}

	core->write (0, 0x28, (uint8_t) ((port << 2) | channelInPort));
}

void OpnVoiceDriver::render (float* out, int frames)
{
	if (core == nullptr || ! core->ready || out == nullptr || frames <= 0)
	{
		return;
	}

	// Two scratch channels, pooled per call rather than per sample: `Update` fills both, and a voice
	// that a song panned to one side would otherwise halve in level when mixed down.
	static thread_local std::vector<DEV_SMPL> left;
	static thread_local std::vector<DEV_SMPL> right;

	if ((int) left.size () < kBlock)
	{
		left.resize ((size_t) kBlock);
		right.resize ((size_t) kBlock);
	}

	int done = 0;

	while (done < frames)
	{
		const int count = std::min (kBlock, frames - done);
		DEV_SMPL* outputs[2] = { left.data (), right.data () };
		core->info.devDef->Update (core->info.dataPtr, (uint32_t) count, outputs);

		for (int i = 0; i < count; ++i)
		{
			const double value = ((double) left[(size_t) i] + (double) right[(size_t) i]) * 0.5;
			out[done + i] = (float) (value * kSamplesToFloat);
		}

		done += count;
	}
}

double OpnVoiceDriver::rate (void) const
{
	if (core != nullptr)
	{
		return core->rate;
	}

	return chips::noteRateFor (loadedModel);
}

} // namespace toybox::backends::libvgm
