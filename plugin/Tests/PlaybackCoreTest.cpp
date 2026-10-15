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

	/** Output buses the routing checks render into; the rest count as "not connected".
		(A namespace-scope constant, so the lambdas below can use it as an array bound.) */
	constexpr int kRoutedBuses = 3;

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

	// ------------------------------------------------- [5] output routing (multi-out)
	std::printf ("[5] output routing\n");

	// The routing checks render into kRoutedBuses buses; the others (up to kOutputBusCount) are
	// simply not handed over, which is exactly how a host reports "this bus is not connected".
	std::vector<std::vector<float>> busBuffersL ((size_t) kRoutedBuses, std::vector<float> ((size_t) blockSize));
	std::vector<std::vector<float>> busBuffersR ((size_t) kRoutedBuses, std::vector<float> ((size_t) blockSize));

	const auto renderBuses = [&] (double seconds)
	{
		std::vector<RenderedBuffer> result ((size_t) kRoutedBuses);
		const int totalSamples = (int) (seconds * sampleRate);
		std::vector<double> sums ((size_t) kRoutedBuses, 0.0);

		vgmplay::playback::OutputBusBuffers buses[kRoutedBuses];
		for (int bus = 0; bus < kRoutedBuses; ++bus)
		{
			buses[bus] = { busBuffersL[(size_t) bus].data(), busBuffersR[(size_t) bus].data() };
		}

		size_t count = 0;
		while ((int) count < totalSamples)
		{
			engine.render (buses, kRoutedBuses, blockSize);

			for (int i = 0; i < blockSize && (int) count < totalSamples; ++i)
			{
				for (int bus = 0; bus < kRoutedBuses; ++bus)
				{
					const double value = (double) busBuffersL[(size_t) bus][(size_t) i];
					sums[(size_t) bus] += value * value;
					result[(size_t) bus].peak = juce::jmax (result[(size_t) bus].peak, std::abs (value));
				}
				++count;
			}
		}

		for (int bus = 0; bus < kRoutedBuses; ++bus)
		{
			result[(size_t) bus].rms = (count > 0) ? std::sqrt (sums[(size_t) bus] / (double) count) : 0.0;
		}
		return result;
	};

	engine.setMixerEnabled (true);
	engine.setPositionSeconds (0.0);
	engine.play();
	const auto baseline = renderBuses (renderLength);
	check (near (baseline[0].rms, expectedAll, 0.002), "everything on the master path fills the main bus");
	check (baseline[1].rms < 1e-9 && baseline[2].rms < 1e-9, "the other buses stay silent");

	// one track straight to "Out 3/4"
	engine.setTrackOutput (0, 1);
	engine.setPositionSeconds (0.0);
	const auto routed = renderBuses (renderLength);
	std::printf ("    track 1 -> Out 3/4: main %.6f, Out 3/4 %.6f\n", routed[0].rms, routed[1].rms);
	check (near (routed[1].rms, kTrackRms, 0.002), "the direct out carries exactly that one track");
	check (routed[0].rms < expectedAll - 0.01, "and that track left the master mix");
	check (near (std::sqrt (routed[0].rms * routed[0].rms
							+ routed[1].rms * routed[1].rms
							+ routed[2].rms * routed[2].rms),
				 expectedAll, 0.002),
		   "all buses together still equal the untouched mix");
	engine.setTrackOutput (0, vgmplay::playback::kMasterOutput);

	// the master mix follows the master's own target
	engine.setMasterOutput (2);
	engine.setPositionSeconds (0.0);
	const auto moved = renderBuses (renderLength);
	check (moved[0].rms < 1e-9 && near (moved[2].rms, expectedAll, 0.002),
		   "the master mix moves to the master's target bus");
	engine.setMasterOutput (0);

	// muting the master silences only the master path
	engine.setTrackOutput (0, 1);
	engine.setMasterMuted (true);
	engine.setPositionSeconds (0.0);
	const auto mutedMaster = renderBuses (renderLength);
	check (mutedMaster[0].rms < 1e-9, "a muted master bus is silent");
	check (near (mutedMaster[1].rms, kTrackRms, 0.002), "while the direct out keeps playing");
	check (engine.getOutputPeak (0) == 0.0f, "and the master meter reads zero");
	engine.setMasterMuted (false);
	engine.setTrackOutput (0, vgmplay::playback::kMasterOutput);

	// a track whose target bus is not connected drops out (bus 5 is not rendered here)
	engine.setTrackOutput (1, 5);
	engine.setPositionSeconds (0.0);
	const auto dropped = renderBuses (renderLength);
	const double expectedRest = kTrackRms * std::sqrt (2.0);	// the two remaining tones
	std::printf ("    track 2 -> an unconnected bus: main %.6f (expected %.6f)\n",
				 dropped[0].rms, expectedRest);
	check (near (dropped[0].rms, expectedRest, 0.002), "a track on an unconnected bus is silent");
	engine.setTrackOutput (1, vgmplay::playback::kMasterOutput);

	// --------------------------------------- [6] the routing scheme new songs start with
	std::printf ("[6] the default routing scheme\n");

	// spreading applies to the loaded song right away ...
	engine.setDefaultRouting (vgmplay::playback::DefaultRouting::spreadAcrossBuses);
	check (engine.getTrackOutput (0) == 0, "spread sends track 1 to Out 1/2");
	check (engine.getTrackOutput (1) == 1, "spread sends track 2 to Out 3/4");
	check (engine.getTrackOutput (2) == 2, "spread sends track 3 to Out 5/6");

	// ... and the next song inherits it (loading the same file again = a fresh track list)
	engine.setPositionSeconds (0.0);
	engine.play();
	check (engine.loadFile (file, error), "the song loads again");
	check (engine.getDefaultRouting() == vgmplay::playback::DefaultRouting::spreadAcrossBuses,
		   "the scheme survived the load");
	check (engine.getTrackOutput (0) == 0 && engine.getTrackOutput (1) == 1,
		   "and the new song starts spread across the buses");

	// switching back to the master scheme applies and sticks the same way
	engine.setDefaultRouting (vgmplay::playback::DefaultRouting::master);
	check (engine.getTrackOutput (0) == vgmplay::playback::kMasterOutput,
		   "sending everything to the master applies at once");
	check (engine.loadFile (file, error), "the song loads once more");
	check (engine.getTrackOutput (0) == vgmplay::playback::kMasterOutput
			   && engine.getTrackOutput (1) == vgmplay::playback::kMasterOutput,
		   "and the new song starts on the master bus");
	engine.setPositionSeconds (0.0);
	engine.play();

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
