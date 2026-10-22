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

namespace
{
	using toybox::chips::OpnDecoder;
	using toybox::chips::RegisterWrite;

	constexpr std::uint32_t kClock = 7670453;	// NTSC Mega Drive
	constexpr std::uint32_t kRate = 44100;

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

		void reg (int port, std::uint8_t reg, std::uint8_t value)
		{
			writeFn (info.dataPtr, (UINT8) (port * 2), reg);
			writeFn (info.dataPtr, (UINT8) (port * 2 + 1), value);
		}

		/** Renders one window with every channel muted but one, and returns that channel's rms. */
		double soloRms (int channel)
		{
			info.devDef->SetMuteMask (info.dataPtr, 0x7Fu & ~(1u << channel));
			std::fill (left.begin(), left.end(), 0);
			std::fill (right.begin(), right.end(), 0);

			DEV_SMPL* outputs[2] = { left.data(), right.data() };
			info.devDef->Update (info.dataPtr, (UINT32) kWindow, outputs);
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
		std::uint8_t algorithm[6] {};
		std::uint8_t level[6][4] {};	// 0x00 = loudest, 0x7F = silent
		bool keyed[6] {};

		// One extra write after the voices are set up: the register-layout cases need an address
		// outside the per-operator loop.
		bool hasExtra = false;
		int extraPort = 0;
		std::uint8_t extraReg = 0;
		std::uint8_t extraValue = 0;
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
		decoder.reset (kClock, (double) kRate);

		auto write = [&] (int port, std::uint8_t reg, std::uint8_t value)
		{
			core.reg (port, reg, value);

			RegisterWrite entry;
			entry.port = (std::uint8_t) port;
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
			const std::uint8_t slot = (std::uint8_t) (channel % 3);

			write (port, (std::uint8_t) (0xB0 + slot), patch.algorithm[channel]);

			for (int op = 0; op < 4; ++op)
			{
				const std::uint8_t base = (std::uint8_t) (op * 4 + slot);
				write (port, (std::uint8_t) (0x30 + base), 0x01);	// MULT 1
				write (port, (std::uint8_t) (0x40 + base), patch.level[channel][op]);
				write (port, (std::uint8_t) (0x50 + base), 0x1F);	// attack: audible at once
				write (port, (std::uint8_t) (0x60 + base), 0x00);	// no decay: the note holds
				write (port, (std::uint8_t) (0x70 + base), 0x00);
				write (port, (std::uint8_t) (0x80 + base), 0x0F);	// sustain 0, release fast
			}

			// A4 on every channel: the pitch is not what this test is about, only the level.
			write (port, (std::uint8_t) (0xA4 + slot), 0x22);		// block 4, f-number high
			write (port, (std::uint8_t) (0xA0 + slot), 0x1D);		// f-number low
		}

		if (patch.hasExtra)
		{
			write (patch.extraPort, patch.extraReg, patch.extraValue);
		}

		for (int channel = 0; channel < 6; ++channel)
		{
			// 0x28 carries the channel and the operator mask; a mask of zero keys that one channel
			// off, which is how a silent channel is written.
			const std::uint8_t slot = (std::uint8_t) (channel % 3);
			const std::uint8_t portBit = (std::uint8_t) (channel >= 3 ? 0x04 : 0x00);
			const std::uint8_t mask = patch.keyed[channel] ? 0xF0 : 0x00;
			write (0, 0x28, (std::uint8_t) (mask | portBit | slot));
		}

		decoder.advance ((std::uint32_t) kWindow);

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
				patch.algorithm[channel] = (std::uint8_t) algorithm;
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
	const std::uint8_t levels[] = { 0x00, 0x20, 0x40, 0x7F };
	for (int index = 0; index < 32; ++index)
	{
		Patch patch;
		for (int channel = 0; channel < 6; ++channel)
		{
			patch.algorithm[channel] = (std::uint8_t) (random() % 8u);
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
