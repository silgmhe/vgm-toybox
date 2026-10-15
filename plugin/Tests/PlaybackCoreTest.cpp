/*
	The playback core test - it links the core (playback/) plus the fake tone backend and **no
	format engine at all**:

	    add_executable(vgm_core_test ... Source/playback/*.cpp Source/backends/fake/FakeBackend.cpp)

	A core that still depended on libvgm would not link here, which is exactly the point: this
	target is the mechanical proof of the decoupling (see docs/playback-backend-design.md).

	On top of that it checks the mixer against signals whose levels are known analytically - the
	fake backend plays three sine tones of amplitude 0.25, so an rms of 0.25/sqrt(2) is one tone
	and 0.25/sqrt(2)*sqrt(3) is all three. That is a much tighter criterion than comparing two
	emulations, and it keeps working when the libvgm backend changes.

	Run: <build dir>/plugin/vgm_core_test
*/

#include <cmath>
#include <cstdio>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>

#include "playback/BackendRegistry.h"
#include "playback/PlaybackEngine.h"

namespace
{
	int failures = 0;

	void check (bool ok, const char* what)
	{
		std::printf ("    %s %s\n", ok ? " OK " : "FAIL", what);
		if (! ok)
		{
			++failures;
		}
	}

	/** RMS of a sine of amplitude 0.25 - the level of exactly one fake track. */
	constexpr double kTrackRms = 0.25 / 1.4142135623730951;

	struct RenderedBuffer
	{
		double rms = 0.0;
		double rmsRight = 0.0;
		double peak = 0.0;
	};

	RenderedBuffer renderSeconds (vgmplay::PlaybackEngine& engine, double seconds, double sampleRate, int blockSize)
	{
		RenderedBuffer result;
		const int totalSamples = (int) (seconds * sampleRate);

		std::vector<float> blockL ((size_t) blockSize), blockR ((size_t) blockSize);
		vgmplay::playback::OutputBusBuffers buses[1] = { { blockL.data(), blockR.data() } };

		double sumL = 0.0;
		double sumR = 0.0;
		size_t count = 0;

		while ((int) count < totalSamples)
		{
			std::fill (blockL.begin(), blockL.end(), 0.0f);
			std::fill (blockR.begin(), blockR.end(), 0.0f);
			engine.render (buses, 1, blockSize);

			for (int i = 0; i < blockSize && (int) count < totalSamples; ++i)
			{
				const double l = (double) blockL[(size_t) i];
				const double r = (double) blockR[(size_t) i];
				sumL += l * l;
				sumR += r * r;
				result.peak = juce::jmax (result.peak, std::abs (l));
				++count;
			}
		}

		result.rms = (count > 0) ? std::sqrt (sumL / (double) count) : 0.0;
		result.rmsRight = (count > 0) ? std::sqrt (sumR / (double) count) : 0.0;
		return result;
	}

	bool near (double value, double expected, double tolerance)
	{
		return std::abs (value - expected) <= tolerance;
	}
}

int main (int /*argc*/, char* /*argv*/[])
{
	constexpr double sampleRate = 48000.0;
	constexpr int blockSize = 256;
	constexpr double renderLength = 0.4;		// shorter than the one second fake song

	// the fake backend only looks at the extension, but loadFile() needs a real file
	const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
						  .getChildFile ("vgmplay_core_test.fake");
	file.replaceWithText ("fake song");

	vgmplay::PlaybackEngine engine;
	engine.prepare (sampleRate, blockSize);

	// ---------------------------------------------------------------- [1] the port
	std::printf ("[1] loading through the playback port\n");

	juce::String error;
	check (engine.loadFile (file, error), "the fake song loads");
	check (error.isEmpty(), "loading reports no error");

	const auto& tracks = engine.getTracks();
	check (tracks.size() == 3, "the backend reports three tracks");
	check (engine.getChips().size() == 2, "the backend reports two chips");
	check (engine.getTag ("Title") == "Fake Song", "the song tags come through");
	check (near (engine.getDurationSeconds(), 1.0, 1e-9), "the duration fact comes through");
	check (near (engine.getLoopPointSeconds(), 0.5, 1e-9), "the loop point fact comes through");

	bool namesOk = ! tracks.empty();
	for (const auto& track : tracks)
	{
		namesOk = namesOk && track.name.isNotEmpty() && track.groupName.isNotEmpty();
	}
	check (namesOk, "every track carries a name and a group");
	check (tracks.size() > 2 && tracks[0].deviceId == 100 && tracks[2].deviceId == 200,
		   "tracks carry their device and group index");

	// ---------------------------------------------------------------- [2] the mix
	std::printf ("[2] the mixer over the fake backend\n");
	check (engine.isMixerEnabled(), "the mixer is on by default");

	engine.setLoopCount (1);
	const auto all = renderSeconds (engine, renderLength, sampleRate, blockSize);
	const double expectedAll = kTrackRms * std::sqrt (3.0);
	std::printf ("    three tones: rms %.6f (expected %.6f)\n", all.rms, expectedAll);
	check (near (all.rms, expectedAll, 0.002), "the mixer sums the tracks at unity gain");

	for (int i = 1; i < 3; ++i)
	{
		engine.setTrackMute ((size_t) i, true);
	}
	engine.setPositionSeconds (0.0);
	const auto one = renderSeconds (engine, renderLength, sampleRate, blockSize);
	std::printf ("    one tone:    rms %.6f (expected %.6f)\n", one.rms, kTrackRms);
	check (near (one.rms, kTrackRms, 0.002), "muting two tracks leaves exactly one tone");

	for (int i = 1; i < 3; ++i)
	{
		engine.setTrackMute ((size_t) i, false);
	}
	engine.setTrackSolo (0, true);
	engine.setPositionSeconds (0.0);
	const auto soloed = renderSeconds (engine, renderLength, sampleRate, blockSize);
	engine.setTrackSolo (0, false);
	check (near (soloed.rms, kTrackRms, 0.002), "solo sounds like muting the other tracks");

	// the remaining checks measure one track, so the others are muted while they run
	for (int i = 1; i < 3; ++i)
	{
		engine.setTrackMute ((size_t) i, true);
	}

	engine.setTrackGain (0, 0.5f);
	engine.setPositionSeconds (0.0);
	const auto halved = renderSeconds (engine, renderLength, sampleRate, blockSize);
	engine.setTrackGain (0, 1.0f);
	check (near (halved.rms / kTrackRms, 0.5, 0.01), "gain 0.5 halves the track");

	engine.setTrackPan (0, -1.0f);
	engine.setPositionSeconds (0.0);
	const auto panned = renderSeconds (engine, renderLength, sampleRate, blockSize);
	engine.setTrackPan (0, 0.0f);
	check (panned.peak > 0.1 && panned.rmsRight < 1e-6, "panning hard left silences the right channel");

	for (size_t i = 0; i < tracks.size(); ++i)
	{
		engine.setTrackMute (i, true);
	}
	engine.setPositionSeconds (0.0);
	const auto muted = renderSeconds (engine, renderLength, sampleRate, blockSize);
	check (muted.rms < 1e-9, "muting every track silences the mixer");
	for (size_t i = 0; i < tracks.size(); ++i)
	{
		engine.setTrackMute (i, false);
	}

	// ------------------------------------------------- [3] the volume stages, seek, end
	std::printf ("[3] the two volume stages, seek and the end of the song\n");

	engine.setMixerMasterGain (0.5f);
	engine.setPositionSeconds (0.0);
	const auto masterHalf = renderSeconds (engine, renderLength, sampleRate, blockSize);
	check (near (masterHalf.rms / all.rms, 0.5, 0.01), "the mixer master scales the whole mix");
	const float meterWithMaster = engine.getOutputPeak (0);

	engine.setMasterVolume (0.5f);
	engine.setPositionSeconds (0.0);
	const auto volumeHalf = renderSeconds (engine, renderLength, sampleRate, blockSize);
	check (near (volumeHalf.rms / masterHalf.rms, 0.5, 0.01),
		   "the output volume is a second, independent stage");
	check (std::abs (engine.getOutputPeak (0) - meterWithMaster) < 0.01f,
		   "the master meter shows the mixer level, not the output volume");

	engine.setMasterVolume (1.0f);
	engine.setMixerMasterGain (1.0f);

	engine.setPositionSeconds (0.4);
	check (near (engine.getPositionSeconds(), 0.4, 0.01), "the seek lands where it was asked to");
	check (engine.getTracks().size() > 1
			   && near (engine.getTrackPositionSeconds (1), 0.4, 0.01),
		   "every track sits on the seek target");

	engine.play();
	renderSeconds (engine, 0.8, sampleRate, blockSize);
	check (engine.isFinished(), "the song reports its end");

	// the meters must not freeze at the last level when the transport stops (no block publishes
	// a peak then - render() returns early, so the engine reports silence instead)
	engine.setMixerEnabled (true);
	engine.setPositionSeconds (0.2);
	engine.play();
	renderSeconds (engine, 0.1, sampleRate, blockSize);
	check (engine.getOutputPeak (0) > 0.01f, "the master meter reads a level while it plays");
	check (engine.getTrackPeak (0, 0) > 0.01f, "the track meters read a level while it plays");

	engine.stop();
	renderSeconds (engine, 0.05, sampleRate, blockSize);	// the audio callback keeps running
	check (engine.getOutputPeak (0) == 0.0f, "stopping clears the master meter");
	check (engine.getTrackPeak (0, 0) == 0.0f, "stopping clears the track meters");

	// -------------------------------------------------------- [4] the mixer switched off
	std::printf ("[4] the mixer switched off (single un-isolated player)\n");

	engine.setMixerEnabled (false);
	engine.setPositionSeconds (0.0);
	engine.play();
	const auto plain = renderSeconds (engine, renderLength, sampleRate, blockSize);
	std::printf ("    plain player: rms %.6f\n", plain.rms);
	check (near (plain.rms, expectedAll, 0.002), "the plain player renders the whole fake song");

	file.deleteFile();

	std::printf ("Result    : %s\n", (failures == 0) ? "all checks passed" : "FAILURES");
	return (failures == 0) ? 0 : 1;
}
