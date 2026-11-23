// **What each FM chip actually reaches**, measured off libvgm's own core.
//
// These are not decoder oracles. An oracle asks two implementations the same question and compares
// their answers; these ask the chip one question - with its loudest patch, how far up the scale does a
// single voice get? - and record the answer as `chips::fmOutputBits`. **Every entry in that table that
// was guessed rather than measured has been wrong**: the YM2612 by four times, the YM2610 by two, the
// YM2151 by four, the YM2413 by sixteen, and the YM3812 by two in the other direction, which clipped.
//
// **Why a file of its own.** They lived in `OpnCoreOracleTest.cpp` because that was the binary that
// already linked libvgm, so a new probe needed no new target - the shortest path, and the wrong one.
// A failure here means "the number in the table is wrong"; a failure there means "the decoder and the
// chip disagree". Two questions, two files. OPL3 and OPX still have to be measured, so the mixed file
// was only going to drift further from its name.
//
//   cmake --build <build dir> --target vgm_fm_level_test
//   <build dir>/plugin/vgm_fm_level_test

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "TestSettings.h"
#include "chips/VoicePatch.h"
#include "playback/NoteWaveform.h"

#include "backends/libvgm/FmVoiceDriver.h"

namespace
{
	int failures = 0;

	void check (bool ok, const char* what)
	{
		std::printf (" %s  %s\n", ok ? "OK  " : "FAIL", what);

		if (! ok)
		{
			++failures;
		}
	}
}		// namespace
/** **What a YM2151 actually reaches, so its output width is measured rather than assumed.**

	The same question the OPN parts each answered differently - 14 bits for the YM2612, 16 for the
	YM2203 and YM2608, 15 for the YM2610 - asked of the OPM. Its patch is four operators with total
	level at zero on all of them, so the peak is the chip's full scale and nothing else.
*/
void testOpmReachesFullScale (void)
{
	auto makePatch = []
	{
		toybox::chips::VoicePatch patch;
		patch.model = toybox::chips::ChipModel::ym2151;

		for (int op = 0; op < 4; ++op)
		{
			const auto base = (uint16_t) (op * 8);
			patch.regs.push_back ({ (uint16_t) (0x40 + base), 0x01 });	// DT1/MUL: ratio 1
			patch.regs.push_back ({ (uint16_t) (0x60 + base), 0x00 });	// TL: full
			patch.regs.push_back ({ (uint16_t) (0x80 + base), 0x1F });	// AR: at once
			patch.regs.push_back ({ (uint16_t) (0xA0 + base), 0x00 });	// D1R
			patch.regs.push_back ({ (uint16_t) (0xC0 + base), 0x00 });	// D2R
			patch.regs.push_back ({ (uint16_t) (0xE0 + base), 0xF0 });	// D1L/RR: loud, slow release
		}

		patch.regs.push_back ({ 0x20, 0xC0 });		// connection 0, both pan bits
		return patch;
	};

	const double wanted = toybox::playback::noteWave::frequencyOf (60);

	struct Rate
	{
		double rate;
		const char* label;
	};

	const Rate rates[] = {
		{ 0.0, "chip" },
		{ 48000.0, "48k" },
	};

	for (const auto& rateCase : rates)
	{
		const auto driver = toybox::backends::libvgm::makeFmVoiceDriver (true, rateCase.rate);
		const auto wave = toybox::playback::noteWave::render (*driver, makePatch (), wanted, 0.6, 0.05,
															 false);

		double peak = 0.0;

		for (const float sample : wave)
		{
			peak = std::max (peak, (double) std::abs (sample));
		}

		std::printf ("    YM2151 %-5s peak %.3f  (rate %.0f, %zu samples)\n", rateCase.label, peak,
					 driver->rate (), wave.size ());

		// **The scale the file will carry.** A declared width six bits too wide leaves the level at a
		// quarter and the quantiser three steps off the core's own LSB - the failure the YM2612 and
		// the YM2610 both had, and this is what tells the OPM apart from them.
		const std::string message = std::string ("the YM2151 reaches full scale at ") + rateCase.label;
		check (peak > 0.9, message.c_str ());
	}
}

/** **What an OPLL actually reaches, so its output width is measured rather than assumed.**

	The user voice (`instrument 0`) with total level zero on both operators and the fastest attack -
	the loudest this chip can be asked for. Its patch is nine registers, not seventeen: an OPLL's
	instruments are a number pointing into the chip's own ROM, and this is the one that is not.
*/
void testOpllReachesFullScale (void)
{
	auto makePatch = []
	{
		toybox::chips::VoicePatch patch;
		patch.model = toybox::chips::ChipModel::ym2413;

		// **The eight bytes are interleaved by register, not by operator** - `EOPLL_patchToDump`
		// (`emu2413.c:1487`) writes dump[0]/[1] from the two operators' 0x00, dump[2]/[3] from their
		// 0x20, and so on. Reading them as "operator 0 then operator 1" (the OPL2 order) leaves every
		// attack rate at zero, which is silence.
		patch.regs.push_back ({ 0x00, 0x01 });		// modulator: AM/PM/EG/VIB/KSR off, multiple 1
		patch.regs.push_back ({ 0x01, 0x01 });		// carrier: the same
		patch.regs.push_back ({ 0x02, 0x00 });		// modulator: KSL 0, TL 0
		patch.regs.push_back ({ 0x03, 0x00 });		// carrier: the same
		patch.regs.push_back ({ 0x04, 0xF0 });		// modulator: attack at once, decay slow
		patch.regs.push_back ({ 0x05, 0xF0 });		// carrier: the same
		patch.regs.push_back ({ 0x06, 0x00 });		// modulator: sustain 0, release slow
		patch.regs.push_back ({ 0x07, 0x00 });		// carrier: the same
		patch.regs.push_back ({ 0x30, 0x00 });		// instrument 0 (the eight above), volume 0 = loudest
		return patch;
	};

	const double wanted = toybox::playback::noteWave::frequencyOf (60);

	struct Rate
	{
		double rate;
		const char* label;
	};

	// **Only the chip's own rate here.** The core resamples internally with two taps at anything
	// else, so a 48 kHz peak would be measuring that converter as much as the core.
	const Rate rates[] = {
		{ 0.0, "chip" },
	};

	for (const auto& rateCase : rates)
	{
		const auto driver = toybox::backends::libvgm::makeFmVoiceDriver (true, rateCase.rate);
		const auto wave = toybox::playback::noteWave::render (*driver, makePatch (), wanted, 0.6, 0.05,
															 false);

		double peak = 0.0;

		for (const float sample : wave)
		{
			peak = std::max (peak, (double) std::abs (sample));
		}

		std::printf ("    YM2413 %-5s peak %.3f  (rate %.0f, %zu samples)\n", rateCase.label, peak,
					 driver->rate (), wave.size ());

		// **The scale the file will carry.** Measured `0.063` at a declared sixteen, which is a
		// sixteenth of the chip's level - the YM2612's failure one octave worse.
		const std::string message = std::string ("the YM2413 reaches full scale at ") + rateCase.label;
		check (peak > 0.9, message.c_str ());
	}
}

/** **What an OPL2 actually reaches.** Sixteen has been the declared width for this chip since the
	table was written and nobody had measured it; the four OPN/OPM entries above show what an
	unmeasured value costs, so this prints the number before anything is asserted about it.

	The patch is the chip's nine registers at their loudest: total level zero on both operators,
	attack rate fifteen, additive connection.
*/
void testOpl2ReachesFullScale (void)
{
	// **A sweep, because one patch is not the chip's ceiling.** The first measurement used additive
	// connection and no feedback - the quietest way to ask for loudness - and every real voice with
	// feedback goes past it. The width has to cover the loudest thing the core can be told to do.
	auto makePatch = [] (int feedback, int connection)
	{
		toybox::chips::VoicePatch patch;
		patch.model = toybox::chips::ChipModel::ym3812;

		patch.regs.push_back ({ 0x20, 0x01 });		// modulator: AM/PM/EG/VIB/KSR off, multiple 1
		patch.regs.push_back ({ 0x40, 0x00 });		// modulator: KSL 0, TL 0
		patch.regs.push_back ({ 0x60, 0xF0 });		// modulator: attack at once
		patch.regs.push_back ({ 0x80, 0x0F });		// modulator: sustain 0, release 15
		patch.regs.push_back ({ 0x23, 0x01 });		// the carrier, the same
		patch.regs.push_back ({ 0x43, 0x00 });
		patch.regs.push_back ({ 0x63, 0xF0 });
		patch.regs.push_back ({ 0x83, 0x0F });
		patch.regs.push_back ({ 0xC0, (uint16_t) ((feedback << 1) | connection) });
		return patch;
	};

	const double wanted = toybox::playback::noteWave::frequencyOf (60);

	const auto driver = toybox::backends::libvgm::makeFmVoiceDriver (true, 0.0);

	double loudest = 0.0;

	for (const int connection : { 0, 1 })
	{
		for (const int feedback : { 0, 7 })
		{
			const auto wave = toybox::playback::noteWave::render (*driver, makePatch (feedback, connection),
																 wanted, 0.6, 0.05, false);

			double peak = 0.0;

			for (const float sample : wave)
			{
				peak = std::max (peak, (double) std::abs (sample));
			}

			std::printf ("    YM3812 con %d fb %d  peak %.3f\n", connection, feedback, peak);
			loudest = std::max (loudest, peak);
		}
	}

	std::printf ("    YM3812 chip  peakiest %.3f  (rate %.0f)\n", loudest, driver->rate ());

	// **The scale the file will carry, and it is not full scale.** libvgm presents this device as
	// int16 and its nine-voice mix fills that range, so one voice belongs at about a ninth of it:
	// the loudest patch here must land near `0x0FFE / 32768 = 0.125`, not at 1.0.
	//
	// Measured against the app on `ym3812/02`: a track renders at `rms 0.069`, and this patch's rms
	// is `~0.05` - the same level, which is the point. A thirteen-bit export was two to three times
	// louder than the timeline, and it took a listener to hear it rather than a fixture.
	check (loudest > 0.10 && loudest < 0.16,
		   "a YM3812 note exports at the level one voice has in the mix");
}

int main()
{
	std::printf ("FM level fixtures: what each chip reaches, measured off libvgm's core\n");

	// Isolated settings - see TestSettings.h.
	const toybox::test::ScopedSettingsDirectory settings;
	(void) settings;

	testOpmReachesFullScale ();
	testOpllReachesFullScale ();
	testOpl2ReachesFullScale ();

	std::printf ("\n  %d check(s) failed\n", failures);
	return failures == 0 ? 0 : 1;
}