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
#include "chips/NoteWaveform.h"

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
		Core()
		{
			DEV_GEN_CFG config {};
			config.emuCore = 0;						// the default core: the one the plugin plays
			config.srMode = DEVRI_SRMODE_CUSTOM;
			config.flags = 0;
			config.clock = kClock;
			config.smplRate = kRate;

			started = SndEmu_Start (DEVID_YM2612, &config, &info) == EERR_OK;
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
			info.devDef->SetMuteMask (info.dataPtr, 0x7Fu & ~(1u << channel));
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
			patch.regs.push_back ({ (uint8_t) (0x40 | base), op == 3 ? 0x00 : 0x7F });
			patch.regs.push_back ({ (uint8_t) (0x50 | base), 0x1A });			// a fast attack
			patch.regs.push_back ({ (uint8_t) (0x60 | base), 0x08 });			// a slower decay
			patch.regs.push_back ({ (uint8_t) (0x70 | base), 0x1F });			// hold
			patch.regs.push_back ({ (uint8_t) (0x80 | base), 0x0A });			// SL 0, release 10
		}

		patch.regs.push_back ({ 0xB0, (uint8_t) (0x07 & algorithm) });
		return patch;
	};

	const auto serial = makePatch (0);
	const auto parallel = makePatch (7);

	const double seconds = toybox::chips::noteWave::durationOf (serial);
	check (seconds > 0.01 && seconds < 20.0, "a note has a plausible length");

	const auto wave = toybox::chips::noteWave::render (serial, toybox::chips::noteWave::frequencyOf (60), seconds, 8000.0);
	check (wave.size() > 100, "the note has samples to draw");

	double peak = 0.0;
	for (float sample : wave)
	{
		peak = std::max (peak, (double) std::abs (sample));
	}
	check (peak > 0.01, "and they are not all silence");
	check (peak <= 1.0, "and stay inside the range the window draws");

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
	check (middle > 0.0, "and it made sound in the middle, so the tail is not silence by accident");

	// Two algorithms, every other register identical: the drawing must be able to tell them apart.
	const auto other = toybox::chips::noteWave::render (parallel, toybox::chips::noteWave::frequencyOf (60), seconds, 8000.0);
	bool differs = other.size() != wave.size();
	if (! differs)
	{
		for (std::size_t i = 0; i < wave.size(); ++i)
		{
			if (std::abs (wave[i] - other[i]) > 0.01f)
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
	// this one is about is the two new fields.
	check (state.voicePatch != nullptr, "the patch behind the channel is handed out");
	check (state.noteWaveform != nullptr, "and a whole note, not just a cycle");

	if (state.noteWaveform != nullptr)
	{
		check (state.noteWaveform->size() > 100,
			   "the note has more than one cycle in it");
	}

	// A second ask with nothing changed hands back the same buffer: the cache is what keeps the
	// panels from rendering eight thousand points thirteen times a frame.
	const auto again = decoder.channelState (0);
	check (again.noteWaveform.get() == state.noteWaveform.get(),
		   "the same patch and note reuse the same waveform");
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

	std::printf ("\n%s\n", failures == 0 ? "All checks passed." : "FAILED.");
	return failures == 0 ? 0 : 1;
}
