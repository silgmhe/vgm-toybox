#include "FmVoiceDriver.h"

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
// The 16-bit scale that used to live here is now computed per model from `chips::fmOutputBits` -
// see `render`. A single constant served the OPN2 and the OPNA alike, and the two do not accumulate
// to the same width.

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
/** **The one FM family whose registers the OPN's sequence must not touch.**

	`load` writes 0x22/0x27/0x2B/0x29 to bring a bare OPN to life. The OPM has none of them: `0x22`
	sits inside its operator banks and `0x27` inside the last channel's, so the writes would corrupt
	two patches rather than configure anything.
*/
constexpr bool isOpm (chips::ChipModel model)
{
	return model == chips::ChipModel::ym2151;
}

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
/** **The libvgm device, in the namespace rather than nested in the class**, because `Dialect`'s
	functions take it and `Dialect` is declared in the header. The header only forward-declares it; its
	members are the engine's own types, which this file exists to keep out of everyone else.
*/
/** **Give a chip its sample banks, silent.** Only the YM2610 needs this - its sole core is the MAME
	one, and it renders nothing at all until the ADPCM banks exist, the FM voices included. Measured
	as `peak 0.000` from a standalone `SndEmu_Start`, which is why its output width could never be
	calibrated from a test.

	`DEV_GEN_CFG` has no ROM field, so this goes through the core's own `rwFuncs`: allocate with
	`DEVRW_MEMSIZE`, then fill with `DEVRW_BLOCK`. Two passes rather than one, because the table lists
	them in the opposite order and the allocation has to come first.
*/
void attachSilentPcmBanks (DEV_INFO& info)
{
	if (info.devDef == nullptr || info.devDef->rwFuncs == nullptr)
	{
		return;
	}

	constexpr uint32_t kBankSize = 0x100000;		// 1 MiB: the ADPCM-A bank's usual size
	const std::vector<uint8_t> silence (kBankSize, 0x00);

	for (const DEVDEF_RWFUNC* rw = info.devDef->rwFuncs; rw->funcPtr != nullptr; ++rw)
	{
		if (rw->funcType == (RWF_MEMORY | RWF_WRITE) && rw->rwType == DEVRW_MEMSIZE)
		{
			reinterpret_cast<DEVFUNC_WRITE_MEMSIZE> (rw->funcPtr) (info.dataPtr, kBankSize);
		}
	}

	for (const DEVDEF_RWFUNC* rw = info.devDef->rwFuncs; rw->funcPtr != nullptr; ++rw)
	{
		if (rw->funcType == (RWF_MEMORY | RWF_WRITE) && rw->rwType == DEVRW_BLOCK)
		{
			reinterpret_cast<DEVFUNC_WRITE_BLOCK> (rw->funcPtr) (info.dataPtr, 0, kBankSize,
																 silence.data ());
		}
	}
}

struct Core
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
			// Only the YM2610 needs banks before it will run - see `attachSilentPcmBanks`.
			attachSilentPcmBanks (info);

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

void opnInit ( Core& core, int channel, chips::ChipModel model )
{
	(void) channel;

	// **The three registers a bare core needs before it will make a sound.** A freshly started YM2612
	// has the LFO and the timers in whatever state the core's constructor leaves them, and channel 3
	// may be in special mode. Without them every note came out silent.
	core.write (0, 0x22, 0x00);		// LFO off: a tremolo would make the level time-dependent
	core.write (0, 0x27, 0x00);		// no timers, channel 3 in normal mode
	core.write (0, 0x2B, 0x00);		// DAC off: this is a voice, not the sample channel

	// **The OPNA and OPNB have to be told they are six-channel parts, and `reset` un-tells them.**
	// 0x29 bit 7 is the "extend 3ch" switch: the core's own reset writes 0x29 = 0x1F, whose bit 7 is
	// clear. The register writes still land in the right place - 0x30|0x100 reaches channel 4 because
	// that path tests the address, not the mode - but 0x28 does not: its bank bit is ignored while
	// the mode is off, so `0xF4` keys channel 1 instead of channel 4.
	if (hasSixChannelMode (model))
	{
		core.write (0, 0x29, sixChannelMode);
	}
}

void opmInit ( Core& core, int channel, chips::ChipModel model )
{
	(void) model;

	// **Pan, and only pan.** `0x20` bits 6 and 7 are L and R, and `render` mixes `(L+R)/2`, so a
	// channel with both clear is silent however good its patch is. Connection 0 keeps the four
	// operators in series, which is what a song's patch normally assumes.
	//
	// **None of the OPN's four registers are written here.** 0x22/0x27/0x2B/0x29 do not exist on this
	// chip: `0x22` is inside the operator banks and `0x27` inside the last channel's, so those writes
	// would corrupt two patches rather than configure anything.
	core.write (0, (uint8_t) (0x20 + channel), 0xC0);
}

void opnNoteOn ( Core& core, int port, int channel, chips::ChipModel model, double hz )
{
	const double clock = (double) clockFor (model);

	// **Per model, and only two of the four are settled.** What is measured, and how:
	//
	//     YM2203  ......................  144 correct at both rates  (testNotePitchIsWhatWasAsked)
	//     YM2608  ......................  288 correct at both rates  (same test, plus the 440 Hz one)
	//     YM2612  panel: neither 144 (130.8 Hz) nor 288 (523.0 Hz) is C4
	//     YM2610  export: unmeasurable (its MAME core gives peak 0.000 without a ROM)
	//
	// YM2612 and YM2610 keep the values a listener confirmed rather than a measurement: 144 for the
	// YM2610 export, and 144 for the YM2612, which is what `ae02342` shipped before this round and
	// what was reported as correct. **The YM2612's panel rate does not match either number**, so its
	// drawn waveform is an octave off in some direction; that is a real open item, deliberately not
	// papered over with a third guess. Fixing it needs the rate passed in rather than inferred, and
	// a measurement of what the core does at 8 kHz - neither of which this change has.
	//
	// YM2608 is `288` here for a measured reason: 144 was an octave low at both rates.
	const double kOpnDivisor = (model == chips::ChipModel::ym2608) ? 288.0 : 144.0;

	int block = 4;
	double fnum = hz * kOpnDivisor * std::exp2 (20.0 - (double) block) / clock;

	while (fnum >= 2048.0 && block < 7)
	{
		++block;
		fnum = hz * kOpnDivisor * std::exp2 (20.0 - (double) block) / clock;
	}

	while (fnum < 1.0 && block > 0)
	{
		--block;
		fnum = hz * kOpnDivisor * std::exp2 (20.0 - (double) block) / clock;
	}

	const auto fnumBits = (uint16_t) std::lround (std::clamp (fnum, 0.0, 2047.0));

	// **0xA4 before 0xA0, and the order is load-bearing.** The chip latches the whole f-number when
	// 0xA0 is written: the low byte from that write, the high bits and block from whatever 0xA4 last
	// held. Writing 0xA0 first locks in high = 0 and block = 0 - a few hertz.
	core.write (port, (uint8_t) (0xA4 + channel),
				(uint8_t) (((fnumBits >> 8) & 0x07) | (block << 3)));
	core.write (port, (uint8_t) (0xA0 + channel), (uint8_t) (fnumBits & 0xFF));

	// **Key on the way a driver does it**, which is three writes and not one:
	//
	//  0x28 = 0x00 | channelKey    first, so the envelope starts from silence.
	//  0x28 = 0xF0 | channelKey    then the actual key.
	//  0xB4 = 0xC0                 and the pan, both sides. **The core starts with it closed**, so
	//                              every note rendered without this line came out at zero.
	//
	// `channelKey` is `(bank << 2) | slot`, not a channel number from 0 to 5.
	const auto channelKey = (uint8_t) ((port << 2) | channel);

	core.write (0, 0x28, channelKey);
	core.write (0, 0x28, (uint8_t) (0xF0 | channelKey));
	core.write (port, (uint8_t) (0xB4 + channel), 0xC0);
}

void opmNoteOn ( Core& core, int port, int channel, chips::ChipModel model, double hz )
{
	(void) port;

	// **The OPM has no f-number.** `0x28` carries the octave in bits 6-4 and a note code in bits 3-0,
	// `0x30` adds 1/64 of a semitone in its top six bits. The code layout skips four values per octave
	// - 3, 7, 11 and 15 repeat the previous semitone - so the inverse needs a table.
	//
	// The forward direction is `OpmNoteTable::noteFor`, which fourteen songs' pitches agree with; this
	// is its other end, and `ChipDecoderTest` holds the two to a round trip.
	(void) model;

	const double semitones = 69.0 + 12.0 * std::log2 (hz / 440.0);
	const double x = semitones - 13.0;		// 0 is C#0, the code table's origin

	const int block = std::clamp ((int) std::floor (x / 12.0), 0, 7);
	const double r = x - 12.0 * (double) block;
	const int delta = std::clamp ((int) std::floor (r), 0, 11);
	const int kf = (int) std::lround ((r - (double) delta) * 64.0);

	// `code - (code >> 2) == delta`, smallest code first.
	static constexpr int kCodeForDelta[12] = { 0, 1, 2, 4, 5, 6, 8, 9, 10, 12, 13, 14 };

	core.write (0, (uint8_t) (0x28 + channel), (uint8_t) ((block << 4) | kCodeForDelta[delta]));
	core.write (0, (uint8_t) (0x30 + channel), (uint8_t) ((kf & 0x3F) << 2));

	// **One key register for the chip**: the channel is in the value's low three bits, 0x78 being
	// "all four operators".
	core.write (0, 0x08, (uint8_t) (0x78 | channel));
}

void opnNoteOff ( Core& core, int port, int channel )
{
	core.write (0, 0x28, (uint8_t) ((port << 2) | channel));
}

void opmNoteOff ( Core& core, int port, int channel )
{
	(void) port;

	// The same address as key-on, with a zero operator mask.
	core.write (0, 0x08, (uint8_t) channel);
}

void opnAfterPatch ( Core&, int )
{
	// Nothing: the OPN's pan is written by `opnNoteOn`.
}

void opmAfterPatch ( Core& core, int channel )
{
	// **After the patch, not before.** The patch carries `0x20|ch` with whatever pan the song chose,
	// and a song that never sets it leaves both bits clear - which is silence, because `render` mixes
	// `(L+R)/2`. The pan is a mix position, not a timbre: forcing both sides changes where the note
	// sits, not what it sounds like.
	core.write (0, (uint8_t) (0x20 + channel), 0xC0);
}

constexpr Dialect kOpnDialect { 0x03, &opnInit, &opnNoteOn, &opnNoteOff, &opnAfterPatch };
constexpr Dialect kOpmDialect { 0x07, &opmInit, &opmNoteOn, &opmNoteOff, &opmAfterPatch };

FmVoiceDriver::FmVoiceDriver (bool exportQualityToUse, double exportRateToUse)
	: exportQuality (exportQualityToUse), exportRate (exportRateToUse)
{
}

FmVoiceDriver::~FmVoiceDriver () = default;

std::unique_ptr<playback::IVoiceDriver> makeFmVoiceDriver (bool exportQuality, double exportRate)
{
	return std::make_unique<FmVoiceDriver> (exportQuality, exportRate);
}

bool FmVoiceDriver::startCore (chips::ChipModel model, double rate)
{
	core = std::make_unique<Core> (rate, model);
	loadedRate = rate;
	loadedModel = model;

	return core != nullptr && core->ready;
}

bool FmVoiceDriver::load (const chips::VoicePatch& patch)
{
	if (patch.regs.empty ())
	{
		return false;
	}

	// Rebuild only when the chip changes. A driver serving six channels of one YM2612 keeps one
	// core; a backend with an OPN and an OPM rebuilds when the patch names a different chip.
	// **The rate is part of "the same chip".** A panel opened after an export would otherwise keep
	// the export's core, because the model had not changed - and every drawn note would be the wrong
	// size.
	//
	// **An export runs at the rate the file will be written at**, not at the chip's own rate. The
	// player does exactly the same thing - it starts every core at the host's rate - and the reason
	// is that resampling afterwards is a step nobody needs: the core times itself from `freqbase`, so
	// the pitch and the timbre are the same at any rate, and only the top of the spectrum moves.
	const double wantedRate = exportQuality
								  ? (exportRate > 0.0 ? exportRate : chips::noteRateFor (patch.model))
								  : chips::kVisualNoteRate;

	if (core == nullptr || ! core->ready || loadedModel != patch.model
		|| loadedRate != wantedRate)
	{
		if (! startCore (patch.model, wantedRate))
		{
			return false;
		}
	}

	// **The dialect follows the model, and it decides the channel mask too.** The channel is the low
	// bits of the addresses the patch came back with; the port is in bit 8 - see VoicePatch.h. The
	// OPN's operator stride is four, so its channel is two bits; the OPM's is eight, so it is three.
	dialect = isOpm (patch.model) ? &kOpmDialect : &kOpnDialect;
	channelInPort = (int) (patch.regs.front ().first & dialect->channelMask);
	port = (patch.regs.front ().first & 0x100) ? 1 : 0;

	// **Start from silence, not from whatever the last channel left.** The core is shared, and the
	// previous channel is still releasing when this patch arrives.
	core->reset ();

	dialect->init (*core, channelInPort, patch.model);

	for (const auto& reg : patch.regs)
	{
		core->write (port, (uint8_t) (reg.first & 0xFF), (uint8_t) reg.second);
	}

	// The OPM's pan has to come after the patch, which carries `0x20|ch` with the song's value.
	dialect->afterPatch (*core, channelInPort);

	return true;
}

void FmVoiceDriver::noteOn (double hz)
{
	if (core == nullptr || ! core->ready || hz <= 0.0)
	{
		return;
	}

	// **The family does the pitch and the key**, in one run because the order matters: the OPN has to
	// write 0xA4 before 0xA0 for the f-number to latch, and the OPM writes KC/KF and then the one key
	// register the chip has. Both are in `FmVoiceDriver.cpp`'s anonymous namespace, beside `Core`.
	dialect->noteOn (*core, port, channelInPort, loadedModel, hz);
}

void FmVoiceDriver::noteOff (void)
{
	if (core == nullptr || ! core->ready)
	{
		return;
	}

	dialect->noteOff (*core, port, channelInPort);
}

void FmVoiceDriver::render (float* out, int frames)
{
	if (core == nullptr || ! core->ready || out == nullptr || frames <= 0)
	{
		return;
	}

	// Two scratch channels, pooled per call rather than per sample: `Update` fills both, and a voice
	// that a song panned to one side would otherwise halve in level when mixed down.
	static thread_local std::vector<DEV_SMPL> left;
	static thread_local std::vector<DEV_SMPL> right;

	// **Scaled by what this chip actually accumulates, not by sixteen bits.** `fmopn` reaches
	// ±0x8000 on the three-channel and six-channel parts but only ±0x2000 on the OPN2 - measured as
	// `peak 0.250` for a YM2612 at the old `1/32768`, against `0.997` for a YM2608. Dividing every
	// model by the same constant therefore did two things to the YM2612 at once: it made an export
	// 12 dB quiet, and it left `noteWave`'s quantiser working at a step four times the core's own
	// LSB - so the quantisation noise sat 12 dB higher relative to the signal than it should.
	// The upper bound is 24, not 16, because one chip needs more than sixteen - see `fmOutputBits`.
	const int bits = chips::fmOutputBits (loadedModel);
	const double scale = 1.0 / (double) (1 << (std::clamp (bits, 2, 24) - 1));

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
			out[done + i] = (float) (value * scale);
		}

		done += count;
	}
}

double FmVoiceDriver::rate (void) const
{
	if (core != nullptr)
	{
		return core->rate;
	}

	return chips::noteRateFor (loadedModel);
}

} // namespace toybox::backends::libvgm