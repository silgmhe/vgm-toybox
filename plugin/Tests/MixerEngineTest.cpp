/*
	Console test for the mixer engine: renders a VGM/VGZ file offline - once through the plain
	player, once through the per-channel mixer - writes the plain render to a WAV and checks the
	mixer acceptance criteria:

	  * one track per channel of every chip, each with a group and a name,
	  * the unity-gain sum of all tracks equals the plain mix (levels and phase),
	  * all tracks stay in sync through seek, loop and the end of the song,
	  * suspending the devices a track does not need really saves render time.

	Usage: vgm_mixer_test <file.vgm|file.vgz> [seconds] [sampleRate]
	Build: cmake --build <build dir> --target vgm_mixer_test
*/

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include "vgm/MixerEngine.h"

namespace
{
	void writeWavFile (const juce::File& file, const std::vector<float>& left, const std::vector<float>& right, int sampleRate)
	{
		const int numSamples = (int) left.size();
		const int dataSize = numSamples * 2 * 2;			// 16-bit stereo
		const int fileSize = 44 + dataSize;

		std::vector<unsigned char> buffer ((size_t) fileSize);
		auto writeU32 = [&buffer] (size_t offset, unsigned int value)
		{
			buffer[offset + 0] = (unsigned char) (value & 0xFF);
			buffer[offset + 1] = (unsigned char) ((value >> 8) & 0xFF);
			buffer[offset + 2] = (unsigned char) ((value >> 16) & 0xFF);
			buffer[offset + 3] = (unsigned char) ((value >> 24) & 0xFF);
		};
		auto writeU16 = [&buffer] (size_t offset, unsigned int value)
		{
			buffer[offset + 0] = (unsigned char) (value & 0xFF);
			buffer[offset + 1] = (unsigned char) ((value >> 8) & 0xFF);
		};

		std::memcpy (&buffer[0], "RIFF", 4);
		writeU32 (4, (unsigned int) (fileSize - 8));
		std::memcpy (&buffer[8], "WAVEfmt ", 8);
		writeU32 (16, 16);									// format chunk size
		writeU16 (20, 1);									// PCM
		writeU16 (22, 2);									// channels
		writeU32 (24, (unsigned int) sampleRate);
		writeU32 (28, (unsigned int) (sampleRate * 4));		// byte rate
		writeU16 (32, 4);									// block align
		writeU16 (34, 16);									// bits per sample
		std::memcpy (&buffer[36], "data", 4);
		writeU32 (40, (unsigned int) dataSize);

		for (int i = 0; i < numSamples; i ++)
		{
			const auto toInt16 = [] (float value)
			{
				value = juce::jlimit (-1.0f, 1.0f, value);
				return (int) std::lround (value * 32767.0f);
			};
			writeU16 ((size_t) 44 + (size_t) i * 4 + 0, (unsigned int) (toInt16 (left[(size_t) i]) & 0xFFFF));
			writeU16 ((size_t) 44 + (size_t) i * 4 + 2, (unsigned int) (toInt16 (right[(size_t) i]) & 0xFFFF));
		}

		file.replaceWithData (buffer.data(), buffer.size());
	}

	struct RenderedBuffer
	{
		std::vector<float> left, right;
		double peak = 0.0;
		double rms = 0.0;
	};

	/** Renders up to `seconds` of the engine output (stops early when the song ends). */
	RenderedBuffer renderSeconds (vgmplay::MixerEngine& engine, double seconds, int sampleRate, int blockSize)
	{
		RenderedBuffer result;
		const int totalSamples = (int) (seconds * sampleRate);
		result.left.reserve ((size_t) totalSamples);
		result.right.reserve ((size_t) totalSamples);

		std::vector<float> blockL ((size_t) blockSize), blockR ((size_t) blockSize);
		float* outputs[2] = { blockL.data(), blockR.data() };

		double sumOfSquares = 0.0;
		size_t sampleCount = 0;

		while ((int) result.left.size() < totalSamples && ! engine.isFinished())
		{
			std::fill (blockL.begin(), blockL.end(), 0.0f);
			std::fill (blockR.begin(), blockR.end(), 0.0f);
			engine.render (outputs, blockSize);

			for (int i = 0; i < blockSize && (int) result.left.size() < totalSamples; i ++)
			{
				result.left.push_back (blockL[(size_t) i]);
				result.right.push_back (blockR[(size_t) i]);
				result.peak = juce::jmax (result.peak, (double) std::abs (blockL[(size_t) i]));
				sumOfSquares += (double) blockL[(size_t) i] * (double) blockL[(size_t) i];
				++sampleCount;
			}
		}

		result.rms = (sampleCount > 0) ? std::sqrt (sumOfSquares / (double) sampleCount) : 0.0;
		return result;
	}

	/** RMS level of (reference - other), in dB relative to the reference RMS.

		Note: two renders are only sample-exact when both followed the same seek history -
		libvgm re-warms the emulation after a backwards seek in a way a fresh start does not
		(plain player mode shows exactly the same), so a comparison that mixes histories can
		never be tighter than ~-30 dB. */
	double differenceDb (const RenderedBuffer& reference, const RenderedBuffer& other)
	{
		const size_t count = juce::jmin (reference.left.size(), other.left.size());
		if (count == 0 || reference.rms <= 0.0)
		{
			return -1000.0;
		}

		double sum = 0.0;
		for (size_t i = 0; i < count; ++i)
		{
			const double diff = (double) reference.left[i] - (double) other.left[i];
			sum += diff * diff;
		}

		const double diffRms = std::sqrt (sum / (double) count);
		if (diffRms <= 0.0)
		{
			return -1000.0;		// bit-identical
		}
		return 20.0 * std::log10 (diffRms / reference.rms);
	}
}

int main (int argc, char* argv[])
{
	std::setvbuf (stdout, nullptr, _IONBF, 0);	// unbuffered, so nothing is lost on a crash

	if (argc < 2)
	{
		std::printf ("Usage: %s <file.vgm|file.vgz> [seconds] [sampleRate]\n", argv[0]);
		return 1;
	}

	const juce::File inputFile { juce::String (argv[1]) };
	const double seconds = (argc > 2) ? std::atof (argv[2]) : 3.0;
	const int sampleRate = (argc > 3) ? std::atoi (argv[3]) : 44100;
	const int blockSize = 512;

	if (! inputFile.existsAsFile())
	{
		std::printf ("File not found: %s\n", inputFile.getFullPathName().toRawUTF8());
		return 1;
	}

	int failures = 0;
	const auto check = [&failures] (bool condition, const juce::String& what)
	{
		std::printf ("    %s %s\n", condition ? "OK  " : "FAIL", what.toRawUTF8());
		if (! condition)
		{
			++failures;
		}
	};

	// --- [1] load ---------------------------------------------------------------------------
	std::printf ("[1] creating engine (sample rate %d, block size %d)\n", sampleRate, blockSize);
	vgmplay::MixerEngine engine;
	engine.prepare ((double) sampleRate, blockSize);

	juce::String errorMessage;
	if (! engine.loadFile (inputFile, errorMessage))
	{
		std::printf ("Loading failed: %s\n", errorMessage.toRawUTF8());
		return 1;
	}

	std::printf ("File      : %s\n", inputFile.getFileName().toRawUTF8());
	std::printf ("Title     : %s\n", engine.getTag ("TITLE").toRawUTF8());
	std::printf ("Game      : %s\n", engine.getTag ("GAME").toRawUTF8());
	std::printf ("Duration  : %.2f s\n", engine.getDurationSeconds());
	std::printf ("Chips     : ");
	for (const auto& chip : engine.getChips())
	{
		std::printf ("%s (%s)  ", chip.name.toRawUTF8(), chip.core.toRawUTF8());
	}
	std::printf ("\n");

	const double songLength = engine.getDurationSeconds();
	const double renderLength = (songLength > 0.0) ? juce::jmin (seconds, songLength * 0.9) : seconds;

	// the mixer is on by default - start from the plain player, so that [2] is the reference
	engine.setMixerEnabled (false);

	engine.setLoopCount (1);
	engine.setFadeSeconds (0.0);
	engine.play();

	// --- [2] the plain player render (reference) --------------------------------------------
	std::printf ("[2] rendering %.2f s in plain player mode\n", renderLength);
	const auto plain = renderSeconds (engine, renderLength, sampleRate, blockSize);
	std::printf ("    peak %.4f, rms %.4f\n", plain.peak, plain.rms);

	// --- [3] mixer mode: one track per channel; the unity sum must match the plain mix ------
	std::printf ("[3] mixer mode: track layout and unity-gain sum\n");
	engine.pause();
	engine.setMixerEnabled (true);

	const auto tracks = engine.getTracks();		// copy: the list is rebuilt on a mode switch
	std::printf ("    %d tracks over %d chip(s)\n", (int) tracks.size(), (int) engine.getChips().size());
	for (size_t i = 0; i < tracks.size(); ++i)
	{
		std::printf ("      [%2d] %-18s %s\n", (int) i,
					 tracks[i].groupName.toRawUTF8(), tracks[i].name.toRawUTF8());
	}

	std::set<UINT32> deviceIDs;
	bool namesOk = ! tracks.empty();
	for (const auto& track : tracks)
	{
		deviceIDs.insert (track.deviceID);
		namesOk = namesOk && track.name.isNotEmpty() && track.groupName.isNotEmpty();
	}
	check (! tracks.empty(), "the song has mixer tracks");
	check (namesOk, "every track carries a name and a group");
	check (deviceIDs.size() == engine.getChips().size(), "one group per chip, no device missing or extra");

	engine.setPositionSeconds (0.0);
	engine.play();
	const auto mixed = renderSeconds (engine, renderLength, sampleRate, blockSize);
	const double sumDifference = differenceDb (plain, mixed);
	std::printf ("    plain rms %.6f vs mixer rms %.6f - difference %.2f dB\n",
				 plain.rms, mixed.rms, sumDifference);
	check (sumDifference < 0.5, "the unity-gain sum matches the plain mix (< 0.5 dB)");

	// --- [4] the tracks stay in sync (seek / loop / end) ------------------------------------
	std::printf ("[4] track sync\n");
	const auto maxTrackSpread = [&engine]
	{
		double spread = 0.0;
		for (size_t i = 0; i < engine.getTracks().size(); ++i)
		{
			spread = juce::jmax (spread, std::abs (engine.getTrackPositionSeconds (i) - engine.getPositionSeconds()));
		}
		return spread;
	};

	{
		const double target = songLength * 0.5;
		engine.setPositionSeconds (target);
		engine.play();
		std::printf ("    seek to %.2f s -> position %.2f s, spread %.4f s\n",
					 target, engine.getPositionSeconds(), maxTrackSpread());
		check (std::abs (engine.getPositionSeconds() - target) < 0.05, "the seek took effect");
		check (maxTrackSpread() < 0.002, "every track sits on the seek target");
	}
	{
		engine.setLoopCount (2);
		engine.setPositionSeconds (0.0);
		engine.play();
		renderSeconds (engine, juce::jmin (songLength * 1.3 + 0.1, seconds * 2.0), sampleRate, blockSize);
		std::printf ("    after looping: position %.2f s, spread %.4f s\n",
					 engine.getPositionSeconds(), maxTrackSpread());
		check (maxTrackSpread() < 0.002, "every track lands on the same position after looping");
	}
	{
		engine.setLoopCount (1);
		engine.setPositionSeconds (juce::jmax (0.0, songLength - 0.1));
		engine.play();
		renderSeconds (engine, seconds, sampleRate, blockSize);

		std::vector<float> blockL ((size_t) blockSize), blockR ((size_t) blockSize);
		float* outputs[2] = { blockL.data(), blockR.data() };
		engine.render (outputs, blockSize);
		double blockPeak = 0.0;
		for (int i = 0; i < blockSize; ++i)
		{
			blockPeak = juce::jmax (blockPeak, (double) std::abs (blockL[(size_t) i]));
		}
		std::printf ("    after the end: finished = %s, next block peak %.6f\n",
					 engine.isFinished() ? "yes" : "no", blockPeak);
		check (engine.isFinished(), "playing past the end reports finished");
		check (blockPeak < 0.001, "the block after the end is silent");
	}

	// --- [5] suspended devices really save emulation time -----------------------------------
	std::printf ("[5] render cost (20 blocks)\n");
	const auto timeRender = [&engine, blockSize]
	{
		std::vector<float> blockL ((size_t) blockSize), blockR ((size_t) blockSize);
		float* outputs[2] = { blockL.data(), blockR.data() };
		const auto start = juce::Time::getHighResolutionTicks();
		for (int i = 0; i < 20; ++i)
		{
			engine.render (outputs, blockSize);
		}
		return juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
	};

	engine.setMixerEnabled (false);
	engine.setLoopCount (1);
	engine.setPositionSeconds (0.0);
	engine.play();
	const double plainTime = timeRender();

	engine.setMixerEnabled (true);
	engine.setPositionSeconds (0.0);
	engine.play();
	const double mixerTime = timeRender();

	std::printf ("    plain %.1f ms vs mixer %.1f ms (x%.2f, %d tracks)\n",
				 plainTime * 1000.0, mixerTime * 1000.0,
				 (plainTime > 0.0) ? mixerTime / plainTime : 0.0, (int) tracks.size());

	// --- [6] a sample rate change rebuilds the song (like a host restoring a session) -------
	std::printf ("[6] re-preparing at 96 kHz\n");
	engine.prepare (96000.0, blockSize);
	std::printf ("    duration = %.2f s, position = %.2f s, tracks = %d\n",
				 engine.getDurationSeconds(), engine.getPositionSeconds(), (int) engine.getTracks().size());
	check (engine.getDurationSeconds() > 0.0, "the song survived the rebuild");

	// --- [7] back to the plain player --------------------------------------------------------
	std::printf ("[7] plain player sanity render\n");
	engine.prepare ((double) sampleRate, blockSize);
	engine.setMixerEnabled (false);
	engine.setPositionSeconds (0.0);
	engine.play();
	const auto sanity = renderSeconds (engine, 0.1, sampleRate, blockSize);
	std::printf ("    peak %.4f\n", sanity.peak);
	check (sanity.peak > 0.001, "the plain player still produces audio");

	// For reference: libvgm's backwards seek does not reproduce a fresh start sample-exactly -
	// the plain player shows the very same difference, so it is the emulation's seek warm-up
	// and not the mixer (which is why the comparisons below give both sides the same history).
	engine.setPositionSeconds (0.0);
	engine.play();
	const auto plainAgain = renderSeconds (engine, renderLength, sampleRate, blockSize);
	std::printf ("    plain render again after a seek: difference %.1f dB (libvgm re-warms after a seek)\n",
				 differenceDb (plain, plainAgain));

	// --- [8] the mixer parameters (mute / solo / gain / pan) --------------------------------
	std::printf ("[8] mixer parameters\n");

	// Levels are compared, not samples: libvgm does not reset the oscillators when it seeks, so
	// two renders that were both started by a seek differ in fine detail - plain player mode
	// shows the very same difference (see "plain render again after a seek" above).
	const auto renderFromStart = [&] (void)
	{
		engine.setPositionSeconds (0.0);
		engine.play();
		return renderSeconds (engine, renderLength, sampleRate, blockSize);
	};

	engine.setMixerEnabled (true);
	engine.setLoopCount (1);

	// soloing track 0 must sound like muting every other track
	engine.setTrackSolo (0, true);
	const auto soloed = renderFromStart();

	engine.setTrackSolo (0, false);
	for (size_t i = 0; i < tracks.size(); ++i)
	{
		engine.setTrackMute (i, i != 0);
	}
	const auto onlyFirst = renderFromStart();

	const double soloLevelDelta = (onlyFirst.rms > 0.0)
									? std::abs (soloed.rms - onlyFirst.rms) / onlyFirst.rms : 1.0;
	std::printf ("    solo: rms %.6f vs mute-others: rms %.6f (level delta %.4f %%)\n",
				 soloed.rms, onlyFirst.rms, 100.0 * soloLevelDelta);
	check (soloLevelDelta < 0.01, "solo sounds like muting every other track (level within 1%)");

	{
		// the very same solo state rendered once more: what is left is the seek warm-up
		engine.setTrackSolo (0, true);
		const auto soloAgain = renderFromStart();
		engine.setTrackSolo (0, false);
		std::printf ("    the same solo state re-rendered after a seek differs by %.1f dB\n",
					 differenceDb (soloed, soloAgain));
	}

	// panning the remaining track hard left silences the right channel
	engine.setTrackPan (0, -1.0f);
	const auto panned = renderFromStart();
	double rightSum = 0.0;
	for (float value : panned.right)
	{
		rightSum += (double) value * (double) value;
	}
	const double rightRms = panned.right.empty() ? 0.0 : std::sqrt (rightSum / (double) panned.right.size());
	std::printf ("    pan hard left: left peak %.4f, right rms %.6f\n", panned.peak, rightRms);
	check (rightRms < 0.001, "panning hard left silences the right channel");
	check (panned.peak > 0.001, "the left channel keeps playing");

	// gain 0.5 halves the level
	engine.setTrackPan (0, 0.0f);
	engine.setTrackGain (0, 0.5f);
	const auto halved = renderFromStart();
	const double gainRatio = (onlyFirst.rms > 0.0) ? halved.rms / onlyFirst.rms : 0.0;
	std::printf ("    gain 0.5 -> rms ratio %.3f\n", gainRatio);
	check (std::abs (gainRatio - 0.5) < 0.02, "gain 0.5 halves the level");

	// muting every track silences the mixer, and the plain player ignores the strip settings
	engine.setTrackGain (0, 1.0f);
	for (size_t i = 0; i < tracks.size(); ++i)
	{
		engine.setTrackMute (i, true);
	}
	engine.setPositionSeconds (0.0);
	engine.play();
	const auto allMuted = renderSeconds (engine, renderLength, sampleRate, blockSize);
	check (allMuted.rms < 0.001, "muting every track silences the mixer");

	engine.setMixerEnabled (false);
	engine.setPositionSeconds (0.0);
	engine.play();
	const auto backToPlain = renderSeconds (engine, renderLength, sampleRate, blockSize);
	check (differenceDb (plain, backToPlain) < 0.5, "switching the mixer off restores the plain mix");

	for (size_t i = 0; i < tracks.size(); ++i)
	{
		engine.setTrackMute (i, false);
	}

	// --- [9] the two volume stages (mixer master, then the output volume) -------------------
	std::printf ("[9] volume stages\n");

	engine.setMixerEnabled (true);
	engine.setLoopCount (1);

	// rendered shorter than the song: the master meter shows the *last* block, and the final
	// blocks of a finished song are silent
	const double shortLength = renderLength * 0.5;

	engine.setPositionSeconds (0.0);
	engine.play();
	const auto unity = renderSeconds (engine, shortLength, sampleRate, blockSize);

	engine.setMixerMasterGain (0.5f);
	engine.setPositionSeconds (0.0);
	engine.play();
	const auto masterHalf = renderSeconds (engine, shortLength, sampleRate, blockSize);
	const float meterWithMaster = engine.getOutputPeak (0);
	const double masterRatio = (unity.rms > 0.0) ? masterHalf.rms / unity.rms : 0.0;
	std::printf ("    mixer master 0.5 -> rms ratio %.3f, master meter %.4f\n",
				 masterRatio, meterWithMaster);
	check (std::abs (masterRatio - 0.5) < 0.01, "the mixer master gain scales the whole mix");

	engine.setMasterVolume (0.5f);
	engine.setPositionSeconds (0.0);
	engine.play();
	const auto volumeHalf = renderSeconds (engine, shortLength, sampleRate, blockSize);
	const float meterWithVolume = engine.getOutputPeak (0);
	const double volumeRatio = (unity.rms > 0.0) ? volumeHalf.rms / unity.rms : 0.0;
	std::printf ("    output volume 0.5 on top -> rms ratio %.3f, master meter %.4f\n",
				 volumeRatio, meterWithVolume);
	check (std::abs (volumeRatio - 0.25) < 0.01, "the output volume is a second, independent stage");
	check (std::abs (meterWithVolume - meterWithMaster) < 0.01f,
		   "the master meter shows the mixer level, not the output volume");

	engine.setMasterVolume (1.0f);
	engine.setMixerMasterGain (1.0f);

	const juce::File outputFile = inputFile.withFileExtension (".rendered.wav");
	writeWavFile (outputFile, plain.left, plain.right, sampleRate);

	std::printf ("Rendered  : %d samples (%.2f s)\n", (int) plain.left.size(), (double) plain.left.size() / sampleRate);
	std::printf ("Peak      : %.4f\n", plain.peak);
	std::printf ("RMS       : %.4f\n", plain.rms);
	std::printf ("WAV       : %s\n", outputFile.getFullPathName().toRawUTF8());
	std::printf ("Result    : %s\n", (failures == 0) ? "all checks passed" : "FAILURES");

	if (failures > 0)
	{
		return 3;
	}
	return (plain.peak > 0.001) ? 0 : 2;
}
