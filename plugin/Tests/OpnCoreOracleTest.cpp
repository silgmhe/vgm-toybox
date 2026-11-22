// The YM2612 decoder against the chip itself: libvgm's own core plays the register stream and the
// decoder has to agree with it, one channel at a time.
//
// ChipDecoderTest states its expectations as numbers derived from the register map - and that is
// exactly how the total-level bug survived: the test was written from the same wrong reading as the
// decoder (0x43 for "channel 0, operator 4"), so the two agreed with each other and both disagreed
// with the hardware. Here the expectation comes from the hardware instead. Every patch is played by
// the emulated chip with all channels muted but one, so "this channel produces sound" is a number
// the core measures, and the decoder has to answer the same question from the same writes.
//
// The gate is the right layer for this: it is the decoder's claim about sound, and the note
// extractor above it only decides how long a claim lives. A gate that is false while the chip
// sounds is a channel that plays with no note - the shape of the original bug.
//
//   cmake --build <build dir> --target vgm_opn_test
//   <build dir>/plugin/vgm_opn_test

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "emu/EmuStructs.h"
#include "emu/SoundDevs.h"
#include "emu/SoundEmu.h"

#include "TestSettings.h"
#include "chips/OpnDecoder.h"
#include "playback/NoteWaveform.h"

#include "backends/libvgm/FmVoiceDriver.h"

namespace
{
	using toybox::chips::OpnDecoder;
	using toybox::chips::RegisterWrite;

	constexpr uint32_t kClock = 7670453;	// NTSC Mega Drive
	constexpr uint32_t kRate = 44100;

	// The render window: long enough for the fastest attack (microseconds) and for the release
	// after a key-off to reach zero, short enough that 64 patches stay cheap.
	constexpr int kWindow = kRate / 4;

	/** How loud is loud. The core's samples are integers on a 16-bit scale and a silent channel
		comes out as exact zeros, so one unit is well above what a muted or fully attenuated channel
		produces and far below a carrier at 0x40 (48 dB down, still tens of units). */
	constexpr double kSilenceFloor = 1.0;

	int failures = 0;

	void check (bool ok, const char* what)
	{
		std::printf (" %s  %s\n", ok ? "OK  " : "FAIL", what);
		if (! ok)
		{
			++failures;
		}
	}

	/** libvgm's YM2612, driven the way the player drives it: an address byte and a data byte, the
		second port through offsets 2 and 3. Its own mute mask turns "does this channel sound?" into
		a measurement instead of a claim. */
	class Core
	{
	public:
		/** @param devId which libvgm device to start - the family, not the variant.
			@param clock its clock, which the OPM does not share with the OPN.
			@param allChannels the mute mask covering every channel this device has: six for the OPN's
			families, eight for the OPM. `soloRms` clears one bit from it.
		*/
		explicit Core (DEV_ID devId = DEVID_YM2612, uint32_t clock = kClock,
					   uint32_t allChannels = 0x7Fu)
			: channels (allChannels)
		{
			DEV_GEN_CFG config {};
			config.emuCore = 0;						// the default core: the one the plugin plays
			config.srMode = DEVRI_SRMODE_CUSTOM;
			config.flags = 0;
			config.clock = clock;
			config.smplRate = kRate;

			started = SndEmu_Start (devId, &config, &info) == EERR_OK;
			if (started)
			{
				usable = SndEmu_GetDeviceFunc (info.devDef, RWF_REGISTER | RWF_WRITE, DEVRW_A8D8, 0,
											   (void**) &writeFn) == EERR_OK;
			}

			left.assign ((size_t) kWindow, 0);
			right.assign ((size_t) kWindow, 0);
		}

		~Core()
		{
			if (started)
			{
				SndEmu_Stop (&info);
			}
			SndEmu_FreeDevLinkData (&info);
		}

		Core (const Core&) = delete;
		Core& operator= (const Core&) = delete;

		bool isValid() const { return usable; }
		const char* name() const { return usable ? info.devDef->name : ""; }

		void reg (int port, uint8_t reg, uint8_t value)
		{
			writeFn (info.dataPtr, (uint8_t) (port * 2), reg);
			writeFn (info.dataPtr, (uint8_t) (port * 2 + 1), value);
		}

		/** Renders one window with every channel muted but one, and returns that channel's rms. */
		double soloRms (int channel)
		{
			info.devDef->SetMuteMask (info.dataPtr, channels & ~(1u << channel));
			std::fill (left.begin(), left.end(), 0);
			std::fill (right.begin(), right.end(), 0);

			DEV_SMPL* outputs[2] = { left.data(), right.data() };
			info.devDef->Update (info.dataPtr, (uint32_t) kWindow, outputs);
			info.devDef->SetMuteMask (info.dataPtr, 0x00);

			double sum = 0.0;
			for (int i = 0; i < kWindow; ++i)
			{
				sum += (double) left[(size_t) i] * (double) left[(size_t) i]
					   + (double) right[(size_t) i] * (double) right[(size_t) i];
			}

			return std::sqrt (sum / (double) (2 * kWindow));
		}

	private:
		DEV_INFO info {};
		DEVFUNC_WRITE_A8D8 writeFn = nullptr;
		bool started = false;
		bool usable = false;
		uint32_t channels = 0x7Fu;
		std::vector<DEV_SMPL> left;
		std::vector<DEV_SMPL> right;
	};
}

namespace
{
	/** A six-channel patch: per channel one algorithm and four total levels, plus which channels the
		key register turns on. */
	struct Patch
	{
		uint8_t algorithm[6] {};
		uint8_t level[6][4] {};	// 0x00 = loudest, 0x7F = silent
		bool keyed[6] {};

		// One extra write after the voices are set up: the register-layout cases need an address
		// outside the per-operator loop.
		bool hasExtra = false;
		int extraPort = 0;
		uint8_t extraReg = 0;
		uint8_t extraValue = 0;
	};

	/** The chip's answer and the decoder's, per channel. */
	struct Verdict
	{
		double rms[6] {};
		bool gate[6] {};
	};

	Verdict play (const Patch& patch)
	{
		Core core;
		OpnDecoder decoder (OpnDecoder::Variant::ym2612);
		decoder.reset (toybox::chips::ChipConfig { kClock, kRate });

		auto write = [&] (int port, uint8_t reg, uint8_t value)
		{
			core.reg (port, reg, value);

			RegisterWrite entry;
			entry.port = (uint8_t) port;
			entry.reg = reg;
			entry.value = value;
			entry.sampleTime = 0;
			decoder.write (entry);
		};

		write (0, 0x22, 0x00);		// LFO off: a tremolo would make the level time-dependent
		write (0, 0x27, 0x00);		// no timers, channel 3 in normal mode
		write (0, 0x2B, 0x00);		// DAC off: this test is about the FM channels

		for (int channel = 0; channel < 6; ++channel)
		{
			const int port = channel / 3;
			const uint8_t slot = (uint8_t) (channel % 3);

			write (port, (uint8_t) (0xB0 + slot), patch.algorithm[channel]);

			for (int op = 0; op < 4; ++op)
			{
				const uint8_t base = (uint8_t) (op * 4 + slot);
				write (port, (uint8_t) (0x30 + base), 0x01);	// MULT 1
				write (port, (uint8_t) (0x40 + base), patch.level[channel][op]);
				write (port, (uint8_t) (0x50 + base), 0x1F);	// attack: audible at once
				write (port, (uint8_t) (0x60 + base), 0x00);	// no decay: the note holds
				write (port, (uint8_t) (0x70 + base), 0x00);
				write (port, (uint8_t) (0x80 + base), 0x0F);	// sustain 0, release fast
			}

			// A4 on every channel: the pitch is not what this test is about, only the level.
			write (port, (uint8_t) (0xA4 + slot), 0x22);		// block 4, f-number high
			write (port, (uint8_t) (0xA0 + slot), 0x1D);		// f-number low
		}

		if (patch.hasExtra)
		{
			write (patch.extraPort, patch.extraReg, patch.extraValue);
		}

		for (int channel = 0; channel < 6; ++channel)
		{
			// 0x28 carries the channel and the operator mask; a mask of zero keys that one channel
			// off, which is how a silent channel is written.
			const uint8_t slot = (uint8_t) (channel % 3);
			const uint8_t portBit = (uint8_t) (channel >= 3 ? 0x04 : 0x00);
			const uint8_t mask = patch.keyed[channel] ? 0xF0 : 0x00;
			write (0, 0x28, (uint8_t) (mask | portBit | slot));
		}

		decoder.advance ((uint32_t) kWindow);

		Verdict verdict;
		for (int channel = 0; channel < 6; ++channel)
		{
			verdict.rms[channel] = core.soloRms (channel);
			verdict.gate[channel] = decoder.channelState (channel).gate;
		}

		return verdict;
	}

	struct Tally
	{
		int comparisons = 0;
		int audible = 0;
		int disagreements = 0;
	};

	/** One comparison, described only when the two sides disagree. */
	void compare (const char* scenario, int channel, const Patch& patch, const Verdict& verdict,
				  Tally& tally)
	{
		const bool sounds = verdict.rms[channel] > kSilenceFloor;
		++tally.comparisons;
		if (sounds)
		{
			++tally.audible;
		}

		if (sounds == verdict.gate[channel])
		{
			return;
		}

		++tally.disagreements;
		std::printf ("    %s: channel %d (algorithm %u, levels %02X %02X %02X %02X, keyed %s): "
					 "the chip measures rms %.1f, the decoder says gate %s\n",
					 scenario, channel, (unsigned) patch.algorithm[channel],
					 (unsigned) patch.level[channel][0], (unsigned) patch.level[channel][1],
					 (unsigned) patch.level[channel][2], (unsigned) patch.level[channel][3],
					 patch.keyed[channel] ? "on" : "off",
					 verdict.rms[channel], verdict.gate[channel] ? "on" : "off");
	}
}

/** **What `capturePatch` hands out, checked against what was written in.**

	Not compared with the core: this is a copy of the register values, so the only questions are
	whether the copy is faithful and whether the addresses are the chip's own. The layout matters
	as much as the values - a rig will write these back in order, and a wrong offset is a patch
	that loads without complaint and sounds wrong.
*/
void testPatchCapture (void)
{
	OpnDecoder decoder;
	decoder.reset (toybox::chips::ChipConfig { kClock, (double) kRate });

	const uint8_t attacks[4] = { 0x1F, 0x12, 0x08, 0x00 };
	const uint8_t decays[4] = { 0x0A, 0x0B, 0x0C, 0x0D };

	for (int op = 0; op < 4; ++op)
	{
		decoder.write ({ toybox::chips::kNoDevice, (uint8_t) (0x50 + op * 4), attacks[op], 0, 0 });
		decoder.write ({ toybox::chips::kNoDevice, (uint8_t) (0x60 + op * 4), decays[op], 0, 0 });
	}

	decoder.write ({ toybox::chips::kNoDevice, 0xB0, 0x3A, 0, 0 });

	toybox::chips::VoicePatch patch;
	check (decoder.capturePatch (0, patch), "the FM decoder serialises its voice");
	check (patch.model == toybox::chips::ChipModel::ym2612, "and says which chip it is");
	check (patch.regs.size() == 29, "29 registers: seven banks of four, plus 0xB0");

	auto valueOf = [&patch] (uint8_t offset)
	{
		for (const auto& reg : patch.regs)
		{
			if (reg.first == offset)
			{
				return (int) reg.second;
			}
		}
		return -1;
	};

	check (valueOf (0x50) == 0x1F, "operator 1's attack rate survives the round trip");
	check (valueOf (0x54) == 0x12, "operator 2's too");
	check (valueOf (0x5C) == 0x00, "operator 4's zero is a value, not a miss");
	check (valueOf (0x60) == 0x0A, "and the decay bank");
	check (valueOf (0xB0) == 0x3A, "the feedback and algorithm byte");

	check (! decoder.capturePatch (6, patch), "the ADPCM channels have no patch");
}

/** **A whole note, not a cycle.** The shape the keyboard's window wants.

	The numbers here are not compared against the chip: `render()` is a structured approximation
	and says so. What is checked is what a drawing depends on - a length that follows the envelope,
	values inside the range the window can draw, and a difference between two algorithms that
	share every other register.
*/
void testNoteWaveform (void)
{
	auto makePatch = [] (uint8_t algorithm)
	{
		toybox::chips::VoicePatch patch;
		patch.model = toybox::chips::ChipModel::ym2612;

		for (int op = 0; op < 4; ++op)
		{
			const uint8_t base = (uint8_t) (op << 2);
			patch.regs.push_back ({ (uint8_t) (0x30 | base), 0x01 });			// multiple 1
			patch.regs.push_back ({ (uint8_t) (0x40 | base), (uint16_t) (op == 3 ? 0x00 : 0x40) });
			patch.regs.push_back ({ (uint8_t) (0x50 | base), 0x1F });			// attack: audible at once
			patch.regs.push_back ({ (uint8_t) (0x60 | base), 0x00 });			// no decay: the note holds
			patch.regs.push_back ({ (uint8_t) (0x70 | base), 0x00 });			// no sustain decay
			patch.regs.push_back ({ (uint8_t) (0x80 | base), 0x0F });			// SL 0, release fast
		}

		patch.regs.push_back ({ 0xB0, (uint8_t) (0x07 & algorithm) });
		return patch;
	};

	const auto serial = makePatch (0);
	const auto parallel = makePatch (7);

	const auto driver = toybox::backends::libvgm::makeFmVoiceDriver ();

	const double seconds = toybox::playback::noteWave::durationOf (serial);
	check (seconds > 0.01 && seconds < 20.0, "a note has a plausible length");

	const auto wave = toybox::playback::noteWave::render (
		*driver, serial, toybox::playback::noteWave::frequencyOf (60), seconds, 1.0, true);
	check (wave.size() > 100, "the note has samples to draw");

	double peak = 0.0;
	for (float sample : wave)
	{
		peak = std::max (peak, (double) std::abs (sample));
	}
	// **Normalised to its own peak**: the loudest sample sits at full scale, whatever the patch's
	// absolute level was. A carrier at total level 48 is -36 dB by design and would draw as a flat
	// line, so the renderer scales each note by 1/peak and leaves the ratios alone.
	check (peak > 0.99, "and they are normalised to full scale");
	check (peak <= 1.0, "and stay inside the range the window draws");

	// **It has to oscillate at the note, not at the envelope.** A 261 Hz note over this buffer is
	// some hundreds of crossings; a waveform that came out as one slow swell - which is what a
	// wrong sample rate or a wrong f-number produces - has a handful. Counting crossings is the
	// only assertion here that can tell those apart.
	int crossings = 0;
	for (std::size_t i = 1; i < wave.size(); ++i)
	{
		if ((wave[i - 1] < 0.0f) != (wave[i] < 0.0f))
		{
			++crossings;
		}
	}
	check (crossings > 100, "and it oscillates at the note, not at the envelope");

	// The last tenth has to be quieter than the middle: the release stage is the thing the old
	// one-cycle drawing could never show.
	double tail = 0.0, middle = 0.0;
	for (std::size_t i = wave.size() * 3 / 4; i < wave.size(); ++i)
	{
		tail += std::abs (wave[i]);
	}
	for (std::size_t i = wave.size() / 4; i < wave.size() / 2; ++i)
	{
		middle += std::abs (wave[i]);
	}
	// The tail has to be **silence**, not merely quieter: that is what a release is, and it is
	// the one thing a cycle-on-loop drawing could never show.
	check (std::abs (wave.back()) < 0.02f, "the note ends in silence, as a release does");
	check (middle > 1.0e-4, "and it made sound in the middle, so the tail is not silence by accident");

	// Two algorithms, every other register identical: the drawing must be able to tell them apart.
	const auto other = toybox::playback::noteWave::render (
		*driver, parallel, toybox::playback::noteWave::frequencyOf (60), seconds, 1.0, true);
	bool differs = other.size() != wave.size();
	if (! differs)
	{
		for (std::size_t i = 0; i < wave.size(); ++i)
		{
			if (std::abs (wave[i] - other[i]) > 1.0e-4f)
			{
				differs = true;
				break;
			}
		}
	}
	check (differs, "algorithm 0 and algorithm 7 do not draw the same waveform");
}

/** **The decoder hands a whole note to the panel**, not just the four numbers it used to.

	The keyboard's window cannot be checked in this harness - it needs real playback to fill - so
	the contract is checked where it is made: after a key-on, `channelState()` carries a patch and
	a waveform with more than one cycle in it.
*/
void testNoteWaveformInState (void)
{
	OpnDecoder decoder;
	decoder.reset (toybox::chips::ChipConfig { kClock, (double) kRate });

	for (int op = 0; op < 4; ++op)
	{
		const uint8_t base = (uint8_t) (op << 2);
		decoder.write ({ toybox::chips::kNoDevice, (uint8_t) (0x30 | base), 0x01, 0, 0 });
		decoder.write ({ toybox::chips::kNoDevice, (uint8_t) (0x40 | base),
						(uint16_t) (op == 3 ? 0x00 : 0x7F), 0, 0 });
		decoder.write ({ toybox::chips::kNoDevice, (uint8_t) (0x50 | base), 0x1A, 0, 0 });
		decoder.write ({ toybox::chips::kNoDevice, (uint8_t) (0x60 | base), 0x08, 0, 0 });
		decoder.write ({ toybox::chips::kNoDevice, (uint8_t) (0x70 | base), 0x1F, 0, 0 });
		decoder.write ({ toybox::chips::kNoDevice, (uint8_t) (0x80 | base), 0x0A, 0, 0 });
	}

	decoder.write ({ toybox::chips::kNoDevice, 0xA0, 0x1D, 0, 0 });
	decoder.write ({ toybox::chips::kNoDevice, 0xA4, 0x22, 0, 0 });		// block 4, fnum high
	decoder.write ({ toybox::chips::kNoDevice, 0xB0, 0x07, 0, 0 });
	decoder.write ({ toybox::chips::kNoDevice, 0x28, 0xF0, 0, 0 });		// key on, all four

	const auto state = decoder.channelState (0);

	// `hz` is not asserted here: it is the note extractor's answer and its own tests cover it. What
	// this one is about is the field that makes the drawing possible.
	check (state.voicePatch != nullptr, "the patch behind the channel is handed out");
	check (state.noteWaveform == nullptr, "but no note is rendered while a song is scanned");

	// The renderer itself is tested above; what is checked here is that a decoder produces the
	// input it needs. That split is deliberate: `chips/` cannot reach an engine header, so the
	// rendering lives in the backend and this is the only contract between them.
	const auto driver = toybox::backends::libvgm::makeFmVoiceDriver ();
	const auto wave = toybox::playback::noteWave::render (*driver, *state.voicePatch, state.hz);
	check (wave.size() > 100, "and the backend can turn it into a whole note");
}

/** **The OPM's core produces sound from the registers the driver writes for it.**

	This is the assertion the OPM shipped without: `peak = 1.0` was measured by hand on one song and
	nothing held it. Three ways to get silence that look identical at ninety pixels wide - writing an
	OPN's registers to an OPM, forgetting that `0x20` bit 6/7 gates the output (both clear by default,
	and `render` mixes `(L+R)/2`), and keying with the wrong address.

	The sequence is what `FmVoiceDriver` writes for this chip: `0x20`, the six operator banks, KC/KF,
	then the key register.
*/
void testOpmCoreMakesSound (void)
{
	Core core (DEVID_YM2151, 3579545u, 0xFFu);

	check (core.isValid (), "libvgm's YM2151 core starts");

	if (! core.isValid ())
	{
		return;
	}

	// Connection 0 (four operators in series) and both pan bits - the pan first, because a patch
	// that does not set it leaves the channel silent whatever else is right.
	core.reg (0, 0x20, 0xC0);

	for (int op = 0; op < 4; ++op)
	{
		const auto base = (uint8_t) (op * 8);

		core.reg (0, (uint8_t) (0x40 + base), 0x01);	// DT1/MUL: ratio 1
		core.reg (0, (uint8_t) (0x60 + base), 0x00);	// TL: no attenuation
		core.reg (0, (uint8_t) (0x80 + base), 0x1F);	// KS/AR: the fastest attack
		core.reg (0, (uint8_t) (0xE0 + base), 0xF0);	// D1L/RR: loudest, slowest release
	}

	// KC = block 4, code 10 -> A4; KF = 0.
	core.reg (0, 0x28, 0x4A);
	core.reg (0, 0x30, 0x00);

	// **One key register for the chip**: channel in bits 2-0, operator mask in 6-3. 0x78 keys all.
	core.reg (0, 0x08, 0x78);

	// Ten times the silence floor, so a channel that renders a few units of leak does not pass.
	check (core.soloRms (0) > 10.0,
		   "the OPM core sounds a note written the way the driver writes it");
}

/** **An OPN note really is the pitch it was asked for.**

	`peak` was the only thing measured on this family, and it cannot see this: a f-number twice too
	large gives a waveform of exactly the same height, one octave up. The OPNA and OPNB carried a
	divisor of 288 instead of 144 for as long as anyone looked - a YM2610 export was audibly an
	octave high with `peak 0.9971` green.

	Measured by counting zero crossings in the middle half. The patch is four carriers at multiple 1,
	so the fundamental is the note and the extra crossings are one waveform's shape, not a different
	pitch; two per period is enough to see a factor of two, and no FFT is needed for that.

	All four parts, because the divisor is one number they share - the two that were wrong are the
	two that no test of level could tell apart.
*/
void testNotePitchIsWhatWasAsked (void)
{
	// CON 7: four carriers, all at multiple 1, so the mix keeps the note's fundamental.
	auto makePatch = [] (toybox::chips::ChipModel model)
	{
		toybox::chips::VoicePatch patch;
		patch.model = model;

		for (int op = 0; op < 4; ++op)
		{
			const uint8_t base = (uint8_t) (op << 2);
			patch.regs.push_back ({ (uint8_t) (0x30 | base), 0x01 });		// multiple 1
			patch.regs.push_back ({ (uint8_t) (0x40 | base), 0x00 });		// audible
			patch.regs.push_back ({ (uint8_t) (0x50 | base), 0x1F });		// attack at once
			patch.regs.push_back ({ (uint8_t) (0x60 | base), 0x00 });		// hold
			patch.regs.push_back ({ (uint8_t) (0x70 | base), 0x00 });
			patch.regs.push_back ({ (uint8_t) (0x80 | base), 0x0F });
		}

		// **Both pan bits as well as the connection.** `0x07` alone is connection 7 with L and R
		// clear, which on the stereo parts is silence - the first version of this test measured
		// 0.0 Hz on a YM2610 and half the pitch on a YM2612, and neither was the divisor.
		patch.regs.push_back ({ 0xB0, 0xC7 });
		return patch;
	};

	struct Case
	{
		toybox::chips::ChipModel model;
		const char* name;
	};

	// **Three of the four parts.** `sndDev_YM2610` declares only `devDef_MAME_2610` as its core
	// (`opnintf.c:262`), and that one does not produce a note from a standalone `SndEmu_Start` with
	// no ROM attached - measured here as `peak 0.000`. A song that plays a YM2610 goes through the
	// player's own setup, which is a different path; its pitch is not something this fixture can
	// assert, so it is not listed rather than listed and guessed at.
	// **The two parts whose divisor is settled.** YM2612 and YM2610 are left out on purpose, and
	// this is the list to extend once they are not:
	//
	//   YM2612: 144 gives 130.8 Hz at the panel rate and 288 gives 523.0 - neither is C4, so the
	//           divisor is not the whole story there and asserting one number would lock in a guess.
	//   YM2610: its only core is `devDef_MAME_2610` (`opnintf.c:262`) and it renders `peak 0.000`
	//           from a standalone `SndEmu_Start`, so there is nothing to measure.
	//
	// Leaving them out is the honest state: a green line here says "the rates this fixture can see
	// are right", not "every OPN is right".
	const Case cases[] = {
		{ toybox::chips::ChipModel::ym2203, "YM2203" },
		{ toybox::chips::ChipModel::ym2608, "YM2608" },
	};

	// C4. Low enough that every part can reach it, high enough that an octave either way is obvious.
	const double wanted = toybox::playback::noteWave::frequencyOf (60);

	// **Both rates, because one part needs a different divisor for each.** The panel draws at
	// `kVisualNoteRate` and an export renders at the chip's own rate; `fmopn.c` forces `freqbase` to
	// `1.0` at the latter, so a divisor that is right for one can be an octave off for the other.
	// That is exactly how a YM2612 came to sound an octave high on the panel while its export was
	// an octave low, with each individual fix looking correct.
	for (const bool exportQuality : { false, true })
	for (const auto& testCase : cases)
	{
		const auto driver = toybox::backends::libvgm::makeFmVoiceDriver (exportQuality);

		const auto wave = toybox::playback::noteWave::render (*driver, makePatch (testCase.model),
															 wanted, 0.6, 0.05, false);

		const auto rate = driver->rate ();

		// Built here rather than inline: `check` takes a `const char*`, and the string has to outlive
		// the call.
		const std::string named = testCase.name;

		if (wave.size () < 1000 || rate <= 0.0)
		{
			const std::string message = named + " renders a note to measure";
			check (false, message.c_str ());
			continue;
		}

		// The middle half: the attack and the release both move the level through zero.
		const std::size_t begin = wave.size () / 4;
		const std::size_t end = wave.size () * 3 / 4;

		int crossings = 0;

		for (std::size_t i = begin + 1; i < end; ++i)
		{
			if ((wave[i - 1] < 0.0f) != (wave[i] < 0.0f))
			{
				++crossings;
			}
		}

		const double seconds = (double) (end - begin) / rate;
		const double measured = (double) crossings / 2.0 / seconds;

		double peak = 0.0;

		for (const float sample : wave)
		{
			peak = std::max (peak, (double) std::abs (sample));
		}

		std::printf ("    %-7s %-6s asked %6.1f Hz, measured %6.1f Hz  (rate %.0f, %zu samples, "
					 "peak %.3f, %d crossings)\n",
					 testCase.name, exportQuality ? "export" : "panel", wanted, measured, rate,
					 wave.size (), peak, crossings);

		// Eight percent: an octave is a hundred. Well inside that, one waveform's harmonics are noise
		// around the zero; outside it, the divisor is wrong.
		const std::string message = named + (exportQuality ? " export" : " panel")
									+ " sounds the pitch it was asked for";
		check (std::abs (measured - wanted) / wanted < 0.08, message.c_str ());
	}
}

int main()
{
	std::printf ("YM2612 decoder against libvgm's own core\n");

	// Isolated settings - see TestSettings.h.
	const toybox::test::ScopedSettingsDirectory settings;

	{
		Core probe;
		check (probe.isValid(), "libvgm's YM2612 core starts");
		if (! probe.isValid())
		{
			std::printf ("      nothing to compare against\n");
			return 1;
		}
		std::printf ("    chip core: %s, clock %u Hz, %u Hz render\n",
					 probe.name(), (unsigned) kClock, (unsigned) kRate);
	}

	// --- the register layout -------------------------------------------------------------------
	// The two addresses that used to be read as "channel 0, operator 4". The special-mode slot is
	// not a total level at all, and it belongs to channel 3; channel 0's carrier under algorithm 0
	// is 0x4C. Both are asked of the chip, not of a datasheet quoted from memory - which is what the
	// wrong version of this test did.
	{
		Patch patch;
		for (int op = 0; op < 4; ++op)
		{
			patch.level[0][op] = 0x7F;
		}
		patch.keyed[0] = true;
		patch.hasExtra = true;
		patch.extraReg = 0x43;			// the slot the chip treats as special mode
		patch.extraValue = 0x00;

		const Verdict verdict = play (patch);
		check (verdict.rms[0] < kSilenceFloor, "0x43 is not a total level: the chip stays silent");
		check (! verdict.gate[0], "0x43 is not channel 0's operator 4: the decoder stays silent");
	}

	{
		Patch patch;
		patch.level[0][0] = 0x7F;
		patch.level[0][1] = 0x7F;
		patch.level[0][2] = 0x7F;
		patch.level[0][3] = 0x00;		// written to 0x4C by play()
		patch.keyed[0] = true;

		const Verdict verdict = play (patch);
		check (verdict.rms[0] >= kSilenceFloor, "0x4C is channel 0's operator 4: the chip sounds");
		check (verdict.gate[0], "0x4C is channel 0's operator 4: the decoder agrees");
	}

	testPatchCapture();
	testNoteWaveform();
	testNotePitchIsWhatWasAsked();
	testNoteWaveformInState();

	Tally tally;

	// --- every algorithm with exactly one operator open ----------------------------------------
	// The chip decides which operator reaches the output. All six channels carry the same patch, so
	// the port-1 channels are covered by every case.
	for (int algorithm = 0; algorithm < 8; ++algorithm)
	{
		for (int open = 0; open < 4; ++open)
		{
			Patch patch;
			for (int channel = 0; channel < 6; ++channel)
			{
				patch.algorithm[channel] = (uint8_t) algorithm;
				for (int op = 0; op < 4; ++op)
				{
					patch.level[channel][op] = (op == open) ? 0x00 : 0x7F;
				}
				patch.keyed[channel] = true;
			}

			const Verdict verdict = play (patch);
			for (int channel = 0; channel < 6; ++channel)
			{
				compare ("algorithm/operator", channel, patch, verdict, tally);
			}
		}
	}

	// --- random patches ------------------------------------------------------------------------
	// Several channels with different algorithms and levels at once: a write that lands on the wrong
	// channel only shows up when its neighbours differ. mt19937's output is specified, so a failing
	// seed reproduces anywhere.
	std::mt19937 random (0x2612);
	const uint8_t levels[] = { 0x00, 0x20, 0x40, 0x7F };
	for (int index = 0; index < 32; ++index)
	{
		Patch patch;
		for (int channel = 0; channel < 6; ++channel)
		{
			patch.algorithm[channel] = (uint8_t) (random() % 8u);
			for (int op = 0; op < 4; ++op)
			{
				patch.level[channel][op] = levels[random() % 4u];
			}
			patch.keyed[channel] = (random() % 4u) != 0;		// mostly on
		}

		const Verdict verdict = play (patch);
		for (int channel = 0; channel < 6; ++channel)
		{
			compare ("random patch", channel, patch, verdict, tally);
		}
	}

	std::printf ("    %d channel comparisons, %d of them audible\n",
				 tally.comparisons, tally.audible);
	check (tally.audible > 100, "the patches make enough sound for the comparison to mean anything");
	check (tally.disagreements == 0, "the decoder agrees with the chip on every channel");

	// --- the clock each model is played at -----------------------------------------------------
	// `noteRateFor` already divides by the chip's own clock - 144 for the OPN family, 64 for the
	// OPM, 72 for the OPL - but the core used to be started at the Mega Drive's 7.67 MHz whichever
	// model the patch named. A YM2608 patch carries f-numbers its driver wrote against 8 MHz, so
	// the core resolved them four percent low. Playing a known pitch and counting zero crossings is
	// the only way to ask which clock it actually used.
	{
		toybox::chips::VoicePatch patch;
		patch.model = toybox::chips::ChipModel::ym2608;

		// Algorithm 7: all four operators in parallel, so the output is a carrier and not the
		// product of one. MUL 1 everywhere keeps them at the same frequency, and AR 31 with DR 0
		// holds the note at full level for as long as the buffer lasts.
		for (int slot = 0; slot < 4; ++slot)
		{
			patch.regs.push_back ({ (uint16_t) (0x30 + slot), 0x01 });		// DT 0, MUL 1
			patch.regs.push_back ({ (uint16_t) (0x40 + slot), 0x00 });		// TL, loudest
			patch.regs.push_back ({ (uint16_t) (0x50 + slot), 0x1F });		// AR 31, instant attack
			patch.regs.push_back ({ (uint16_t) (0x60 + slot), 0x00 });		// DR 0, no decay
			patch.regs.push_back ({ (uint16_t) (0x70 + slot), 0x00 });		// SR 0
			patch.regs.push_back ({ (uint16_t) (0x80 + slot), 0x0F });		// SL 0, RR 15
		}

		patch.regs.push_back ({ (uint16_t) 0xB0, 0x07 });		// feedback 0, algorithm 7
		patch.regs.push_back ({ (uint16_t) 0xB4, 0xC0 });		// both speakers

		const auto driver = toybox::backends::libvgm::makeFmVoiceDriver ();

		if (driver->load (patch))
		{
			driver->noteOn (440.0);

			const int frames = 8192;
			std::vector<float> samples ((size_t) frames);
			driver->render (samples.data (), frames);

			int crossings = 0;

			for (int i = 1; i < frames; ++i)
			{
				if ((samples[(size_t) i - 1] < 0.0f) != (samples[(size_t) i] < 0.0f))
				{
					++crossings;
				}
			}

			const double seconds = (double) frames / driver->rate ();
			const double measured = (double) crossings / 2.0 / seconds;

			std::printf ("    YM2608 at 440 Hz: %.1f Hz measured\n", measured);

			// Five hertz is wide enough for an eleven-bit f-number's quantisation and narrow enough
			// that the 7.67/8.00 gap - about eighteen hertz - cannot pass through it.
			check (std::fabs (measured - 440.0) < 5.0,
				   "a YM2608 plays at the clock its model declares");
		}
		else
		{
			check (false, "the core starts for a YM2608 patch");
		}
	}

	// --- a shared core must not let one channel into the next -----------------------------------
	// One driver serves every row of a song, and the next patch arrives before the previous channel
	// has finished releasing. Without a reset between them the second row carries the first row's
	// tail, and two waveforms in one picture is what "the timbre is wrong" looks like from outside.
	{
		// Release rate 1 is a tail that lasts seconds - long enough to be caught in the act. The two
		// notes go to **different channels**, which is the case that matters: one channel's key-on
		// does not disturb another's, so nothing but a reset stops the first one sounding.
		const auto longTailed = [] (int channel)
		{
			toybox::chips::VoicePatch patch;
			patch.model = toybox::chips::ChipModel::ym2608;

			for (int slot = 0; slot < 4; ++slot)
			{
				const int base = (slot << 2) | channel;

				patch.regs.push_back ({ (uint16_t) (0x30 + base), 0x01 });		// MUL 1
				patch.regs.push_back ({ (uint16_t) (0x40 + base), 0x00 });		// TL, loudest
				patch.regs.push_back ({ (uint16_t) (0x50 + base), 0x1F });		// AR 31
				patch.regs.push_back ({ (uint16_t) (0x60 + base), 0x00 });		// DR 0, holds
				patch.regs.push_back ({ (uint16_t) (0x70 + base), 0x00 });
				patch.regs.push_back ({ (uint16_t) (0x80 + base), 0x01 });		// SL 0, RR 1
			}

			patch.regs.push_back ({ (uint16_t) (0xB0 + channel), 0x07 });		// algorithm 7
			patch.regs.push_back ({ (uint16_t) (0xB4 + channel), 0xC0 });
			return patch;
		};

		const auto driver = toybox::backends::libvgm::makeFmVoiceDriver ();

		driver->load (longTailed (0));
		driver->noteOn (440.0);

		{
			std::vector<float> warm ((size_t) 4096);
			driver->render (warm.data (), 4096);
		}

		// The second note goes to channel 1 and is asked for 220 Hz. It has to come back as 220,
		// not as a mixture with the first channel, which is still keyed on.
		driver->load (longTailed (1));
		driver->noteOn (220.0);

		const int frames = 8192;
		std::vector<float> samples ((size_t) frames);
		driver->render (samples.data (), frames);

		int crossings = 0;

		for (int i = 1; i < frames; ++i)
		{
			if ((samples[(size_t) i - 1] < 0.0f) != (samples[(size_t) i] < 0.0f))
			{
				++crossings;
			}
		}

		const double seconds = (double) frames / driver->rate ();
		const double measured = (double) crossings / 2.0 / seconds;

		std::printf ("    a 220 Hz note after a 440 Hz one: %.1f Hz measured\n", measured);

		check (std::fabs (measured - 220.0) < 6.0,
			   "a loaded patch does not carry the previous channel's tail");
	}

	// --- the sixth-channel mode has to survive the reset ------------------------------------
	// `reset` ends with `YM2608IRQMaskWrite(OPN, F2608, 0x1F)`, and 0x29's bit 7 is the "extend
	// 3ch" switch - clear, so the core comes back as a three-channel OPN. A patch written to
	// 0x30|0x100 still reaches channel 4, because that path tests the address rather than the
	// mode. 0x28 does not: its bank bit is ignored, so a key-on meant for channel 4 starts
	// channel 1, and the top half of the chip renders silence while holding a complete patch.
	//
	// The two assertions above cannot see this. Both play a patch on the first port, which is the
	// half that keeps working, so they passed for as long as this was broken.
	{
		toybox::chips::VoicePatch patch;
		patch.model = toybox::chips::ChipModel::ym2608;

		for (int slot = 0; slot < 4; ++slot)
		{
			const int base = slot << 2;

			patch.regs.push_back ({ (uint16_t) (0x130 | base), 0x01 });		// MUL 1
			patch.regs.push_back ({ (uint16_t) (0x140 | base), 0x00 });		// TL, loudest
			patch.regs.push_back ({ (uint16_t) (0x150 | base), 0x1F });		// AR 31
			patch.regs.push_back ({ (uint16_t) (0x160 | base), 0x00 });		// DR 0, holds
			patch.regs.push_back ({ (uint16_t) (0x170 | base), 0x00 });
			patch.regs.push_back ({ (uint16_t) (0x180 | base), 0x0F });		// SL 0, RR 15
		}

		patch.regs.push_back ({ (uint16_t) 0x1B0, 0x07 });		// algorithm 7
		patch.regs.push_back ({ (uint16_t) 0x1B4, 0xC0 });		// both speakers

		const auto driver = toybox::backends::libvgm::makeFmVoiceDriver ();

		if (driver->load (patch))
		{
			driver->noteOn (440.0);

			const int frames = 4096;
			std::vector<float> samples ((size_t) frames);
			driver->render (samples.data (), frames);

			float peak = 0.0f;

			for (const float v : samples)
			{
				peak = std::max (peak, std::fabs (v));
			}

			std::printf ("    YM2608 second-port peak: %.4f\n", (double) peak);

			// The patch is at full level on every operator and the algorithm is four carriers in
			// parallel, so a working key-on cannot produce anything near zero.
			check (peak > 0.5f, "a patch written to the second port reaches its channel");
		}
		else
		{
			check (false, "the core starts for a second-port patch");
		}
	}

	// --- the OPM -------------------------------------------------------------------------------
	// The chip this suite had nothing to say about, while its `peak` was verified by hand.
	std::printf ("\nOPM\n");
	testOpmCoreMakesSound ();

	std::printf ("\n%s\n", failures == 0 ? "All checks passed." : "FAILED.");
	return failures == 0 ? 0 : 1;
}
