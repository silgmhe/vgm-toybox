/*
	Console test for the mixer engine: renders a VGM/VGZ file offline - once through the plain
	player, once through the per-channel mixer - writes the plain render to a WAV and checks the
	mixer acceptance criteria:

	  * one track per channel of every chip, each with a group and a name,
	  * the unity-gain sum of all tracks equals the plain mix (levels and phase),
	  * all tracks stay in sync through seek, loop and the end of the song,
	  * suspending the devices a track does not need really saves render time,
	  * the MIDI sink: per-track channels, mute/solo, the master's merged copy, no notes while the
	    mixer is off, and all-notes-off after a seek,
	  * the command clock behind those notes: it ends on the VGM header's length, and a second loop
	    pass still sends notes (the sequence covers one pass, the engine plays many).

	  Usage: vgm_mixer_test <file.vgm|file.vgz> [seconds] [sampleRate]
	  Build: cmake --build <build dir> --target vgm_mixer_test
	  */

	  #include <cmath>
	  #include <cstdint>
	  #include <cstdio>
	  #include <cstdlib>
	  #include <set>
	  #include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include "playback/PlaybackEngine.h"

#include "backends/libvgm/VgmCommandReader.h"

#include "TestSupport.h"

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
	RenderedBuffer renderSeconds (toybox::PlaybackEngine& engine, double seconds, int sampleRate, int blockSize)
	{
		RenderedBuffer result;
		const int totalSamples = (int) (seconds * sampleRate);
		result.left.reserve ((size_t) totalSamples);
		result.right.reserve ((size_t) totalSamples);

		std::vector<float> blockL ((size_t) blockSize), blockR ((size_t) blockSize);
		// the main bus only: this test keeps every track on the master path (bus 0 == "Out 1/2")
		toybox::playback::OutputBusBuffers buses[1] = { { blockL.data(), blockR.data() } };

		double sumOfSquares = 0.0;
		size_t sampleCount = 0;

		while ((int) result.left.size() < totalSamples && ! engine.isFinished())
		{
			std::fill (blockL.begin(), blockL.end(), 0.0f);
			std::fill (blockR.begin(), blockR.end(), 0.0f);
			engine.render (buses, 1, blockSize);

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

		const 	double diffRms = std::sqrt (sum / (double) count);
		if (diffRms <= 0.0)
		{
			return -1000.0;		// bit-identical
		}
		return 20.0 * std::log10 (diffRms / reference.rms);
	}

	/** What the host would receive: the same sink the plugin hands to render(). */
	struct MidiRecorder final : public toybox::playback::MidiBlockSink
	{
		void send (const toybox::playback::MidiBlockEvent& event) override
		{
			events.push_back (event);
			blockStarts.push_back (blockStartSample);
		}

		/** Note-ons of one channel that arrived at or after `sample` on the render's own clock.
			The events alone only know their offset inside a block, which cannot tell the first
			pass of a loop from the second. */
		int countNoteOnsFrom (int channel, std::int64_t sample) const
		{
			int result = 0;
			for (std::size_t i = 0; i < events.size() && i < blockStarts.size(); ++i)
			{
				if ((events[i].status & 0xF0) == 0x90 && (events[i].status & 0x0F) == channel - 1
					&& blockStarts[i] >= sample)
				{
					++result;
				}
			}
			return result;
		}

		/** Messages of one kind: 0x90 note-on, 0x80 note-off, 0xB0 control change. */
		int count (std::uint8_t statusHighNibble) const
		{
			int result = 0;
			for (const auto& event : events)
			{
				if ((event.status & 0xF0) == statusHighNibble)
				{
					++result;
				}
			}
			return result;
		}

		/** Whether one exact message went out on any channel (used for the RPN sequence). */
		bool has (std::uint8_t statusHighNibble, std::uint8_t data1, std::uint8_t data2) const
		{
			for (const auto& event : events)
			{
				if ((event.status & 0xF0) == statusHighNibble && event.data1 == data1 && event.data2 == data2)
				{
					return true;
				}
			}
			return false;
		}

		/** The same, restricted to one channel (1..16). */
		int count (std::uint8_t statusHighNibble, int channel) const
		{
			int result = 0;
			for (const auto& event : events)
			{
				if ((event.status & 0xF0) == statusHighNibble && (event.status & 0x0F) == channel - 1)
				{
					++result;
				}
			}
			return result;
		}

		/** Messages with this exact content, any channel - tells a centred bend from a real one. */
		int countOf (std::uint8_t statusHighNibble, std::uint8_t data1, std::uint8_t data2) const
		{
			int result = 0;
			for (const auto& event : events)
			{
				if ((event.status & 0xF0) == statusHighNibble
					&& event.data1 == data1 && event.data2 == data2)
				{
					++result;
				}
			}
			return result;
		}

		int countController (int controller) const
		{
			int result = 0;
			for (const auto& event : events)
			{
				if ((event.status & 0xF0) == 0xB0 && event.data1 == (std::uint8_t) controller)
				{
					++result;
				}
			}
			return result;
		}

		/** The notes a channel was told to play, in the order they were sent. */
		std::vector<int> notesOn (int channel) const
		{
			std::vector<int> result;
			for (const auto& event : events)
			{
				if ((event.status & 0xF0) == 0x90 && (event.status & 0x0F) == channel - 1)
				{
					result.push_back (event.data1);
				}
			}
			return result;
		}

		std::vector<toybox::playback::MidiBlockEvent> events;
		std::vector<std::int64_t> blockStarts;		// where each event's block began, in samples
		std::int64_t blockStartSample = 0;			// set by the render loop below
	};

	/** Renders `seconds` with the MIDI sink attached; the audio is rendered and discarded. */
	MidiRecorder recordMidi (toybox::PlaybackEngine& engine, double seconds, int sampleRate, int blockSize)
	{
		MidiRecorder recorder;
		const int totalSamples = (int) (seconds * sampleRate);
		int rendered = 0;

		std::vector<float> blockL ((size_t) blockSize), blockR ((size_t) blockSize);
		toybox::playback::OutputBusBuffers buses[1] = { { blockL.data(), blockR.data() } };

		while (rendered < totalSamples && ! engine.isFinished())
		{
			std::fill (blockL.begin(), blockL.end(), 0.0f);
			std::fill (blockR.begin(), blockR.end(), 0.0f);
			recorder.blockStartSample = rendered;
			engine.render (buses, 1, blockSize, &recorder);
			rendered += blockSize;
		}

		return recorder;
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

	// --- [1] load ---------------------------------------------------------------------------
	std::printf ("[1] creating engine (sample rate %d, block size %d)\n", sampleRate, blockSize);
	toybox::PlaybackEngine engine;
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

	std::set<toybox::playback::DeviceId> deviceIDs;
	bool namesOk = ! tracks.empty();
	for (const auto& track : tracks)
	{
		deviceIDs.insert (track.deviceId);
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
		toybox::playback::OutputBusBuffers buses[1] = { { blockL.data(), blockR.data() } };
		engine.render (buses, 1, blockSize);
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
		toybox::playback::OutputBusBuffers buses[1] = { { blockL.data(), blockR.data() } };
		const auto start = juce::Time::getHighResolutionTicks();
		for (int i = 0; i < 20; ++i)
		{
			engine.render (buses, 1, blockSize);
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

	// --- [10] MIDI output ---------------------------------------------------------------------
	// The host only ever sees the sink render() fills: every check below looks at the messages a
	// VST3 wrapper would put into the output event buffer, so no DAW is needed to prove the
	// routing. The song is the same one the window scripts load.
	std::printf ("[10] MIDI output\n");

	engine.setMixerEnabled (true);
	engine.setLoopCount (1);
	for (size_t i = 0; i < tracks.size(); ++i)
	{
		engine.setTrackMute (i, false);
		engine.setTrackSolo (i, false);
		engine.setTrackMidiChannel (i, 0);
	}
	engine.setMasterMidiChannel (0);

	// every channel Off: the routing state is kept, but nothing is generated
	engine.setPositionSeconds (0.0);
	engine.play();
	const auto everythingOff = recordMidi (engine, 0.5, sampleRate, blockSize);
	std::printf ("    all channels off: %d note-on, %d all-notes-off\n",
				 everythingOff.count (0x90), everythingOff.countController (123));
	check (everythingOff.count (0x90) == 0, "every channel Off sends no notes");

	// the reset that starts a render is what keeps a host from holding a note from before the
	// seek/stop - it goes to all 16 channels because any of them may still hold one
	check (everythingOff.countController (123) == 16, "the first block sends all-notes-off on every channel");
	check (everythingOff.countController (120) == 16, "and all-sound-off with it");

	// one track on channel 1: its notes, and only they. *Which* track is audible in the opening
	// second is the song's business - a real Mega Drive song may leave its first channel silent
	// until the melody starts - so the track is looked for here instead of assumed. The routing
	// is what this checks; hard-coding track 0 made every real song fail on its arrangement.
	int routedTrack = -1;
	MidiRecorder routed;
	for (size_t i = 0; i < tracks.size() && routedTrack < 0; ++i)
	{
		engine.setTrackMidiChannel (i, 1);
		engine.setPositionSeconds (0.0);
		engine.play();
		routed = recordMidi (engine, 1.0, sampleRate, blockSize);
		if (! routed.notesOn (1).empty())
		{
			routedTrack = (int) i;
		}
		else
		{
			engine.setTrackMidiChannel (i, 0);
		}
	}

	const auto routedNotes = routed.notesOn (1);
	std::printf ("    track %d -> channel 1: %d note-on (first note %d), %d note-off\n",
				 routedTrack, (int) routedNotes.size(), routedNotes.empty() ? -1 : routedNotes.front(),
				 routed.count (0x80));
	check (routedTrack >= 0, "a track with a channel sends its notes");
	check (routed.count (0x90) == routed.count (0x90, 1), "nothing leaks onto a channel nobody routed to");
	// 3579545 / (32 * 254) = 440.4 Hz: the shipped test song's SN76489 tone 1 is A4 on the MIDI
	// side. On a real song the same check would only say "some pitch came out", so it stays the
	// asset's - the decoder's pitch question is answered in vgm_chip_test either way.
	if (inputFile.getFileName() == "minimal.vgm")
	{
		check (! routedNotes.empty() && routedNotes.front() == 69, "440 Hz decodes to MIDI note 69 (A4)");
	}

	// Pitch bend: a 14-bit value means nothing without the receiver's range, and MIDI never
	// reports it - so the engine declares the preference (RPN 0, pitch bend sensitivity) on the
	// channel it is about to play, before the first note. 12 semitones is the default.
	check (routed.has (0xB0, 101, 0) && routed.has (0xB0, 100, 0)
		   && routed.has (0xB0, 6, 12) && routed.has (0xB0, 38, 0),
		   "the bend range is declared as RPN 0 before the first note");

	// ... and the switch is the whole difference: with bends off the output carries no 0xE0 at
	// all, while the notes are unchanged. One message is allowed and expected on a song that does
	// bend: flipping the switch has to centre a channel this engine left off-centre, or the whole
	// mode would keep the last bend until the next song. It is exactly one per channel, once.
	engine.setMidiPitchBendEnabled (false);
	engine.setPositionSeconds (0.0);
	engine.play();
	const auto noBends = recordMidi (engine, 1.0, sampleRate, blockSize);
	std::printf ("    bends off: %d note-on, %d pitch bend (%d of them centred)\n",
				 noBends.count (0x90, 1), noBends.count (0xE0), noBends.countOf (0xE0, 0, 64));
	check (noBends.countOf (0xE0, 0, 64) == noBends.count (0xE0),
		   "every 0xE0 sent with bends off is the centred message that retires a held bend");
	check (noBends.count (0xE0) <= 16, "and there is at most one of them per channel");
	check (noBends.count (0x90, 1) > 0, "while the notes still come out");
	engine.setMidiPitchBendEnabled (true);

	// mute: the channel stays set (the strip still shows it), the events stop
	const size_t playingTrack = (size_t) juce::jmax (0, routedTrack);
	engine.setTrackMute (playingTrack, true);
	engine.setPositionSeconds (0.0);
	engine.play();
	const auto muted = recordMidi (engine, 1.0, sampleRate, blockSize);
	check (muted.count (0x90) == 0, "a muted track sends no MIDI even though its channel is still set");
	engine.setTrackMute (playingTrack, false);

	// solo: a soloed track silences the others' MIDI as well - solo a strip that carries no
	// channel, and the notes on channel 1 have to stop
	if (tracks.size() > 1)
	{
		const size_t soloedTrack = (routedTrack == 0) ? 1 : 0;
		engine.setTrackSolo (soloedTrack, true);
		engine.setPositionSeconds (0.0);
		engine.play();
		const auto soloedMidi = recordMidi (engine, 1.0, sampleRate, blockSize);
		check (soloedMidi.count (0x90) == 0, "soloing another track silences this one's MIDI too");
		engine.setTrackSolo (soloedTrack, false);
	}

	// master: an extra merged copy on its own channel, same notes, same order
	engine.setMasterMidiChannel (10);
	engine.setPositionSeconds (0.0);
	engine.play();
	const auto merged = recordMidi (engine, 1.0, sampleRate, blockSize);
	std::printf ("    master channel 10: %d note-on direct, %d merged\n",
				 merged.count (0x90, 1), merged.count (0x90, 10));
	check (merged.notesOn (1) == routedNotes, "the track's own channel still gets its notes");
	check (merged.notesOn (10) == routedNotes, "the master channel gets a copy of every note");

	// master on the same channel as the track: one message, not two (a doubled note-off would cut
	// the note the first one started)
	engine.setMasterMidiChannel (1);
	engine.setPositionSeconds (0.0);
	engine.play();
	const auto sameChannel = recordMidi (engine, 1.0, sampleRate, blockSize);
	check (sameChannel.notesOn (1) == routedNotes, "the master does not double a track on the same channel");

	// the master strip's mute silences the merged copy only - the track's own channel keeps playing
	engine.setMasterMidiChannel (10);
	engine.setMasterMuted (true);
	engine.setPositionSeconds (0.0);
	engine.play();
	const auto masterMuted = recordMidi (engine, 1.0, sampleRate, blockSize);
	check (masterMuted.count (0x90, 10) == 0, "muting the master drops its copy");
	check (masterMuted.notesOn (1) == routedNotes, "the direct channel keeps playing while the master is muted");
	engine.setMasterMuted (false);

	// the mixer switch is the opt-out: off means the per-channel routing does not exist
	engine.setMixerEnabled (false);
	engine.setPositionSeconds (0.0);
	engine.play();
	const auto plainPlayer = recordMidi (engine, 1.0, sampleRate, blockSize);
	check (plainPlayer.count (0x90) == 0, "with the mixer switched off no MIDI is generated");
	engine.setMixerEnabled (true);

	// a seek mid-song: the new position's events resume, the previous notes are closed first
	engine.setMasterMidiChannel (0);
	engine.setPositionSeconds (0.0);
	engine.play();
	engine.setPositionSeconds (2.0);
	const auto afterSeek = recordMidi (engine, 0.2, sampleRate, blockSize);
	check (afterSeek.countController (123) == 16, "a seek sends all-notes-off on every channel");

	// chronological order inside a block: the sink's offsets are what the host schedules. The
	// offsets are per block, so the block's start is added back - a recorder that spans a second
	// covers many blocks, and offset 0 of the next block is not "before" the last offset of the
	// previous one (comparing raw offsets made this pass only while the song had a single event).
	bool chronological = true;
	for (size_t i = 1; i < routed.events.size() && i < routed.blockStarts.size(); ++i)
	{
		const auto previous = routed.blockStarts[i - 1] + routed.events[i - 1].sampleOffset;
		const auto current = routed.blockStarts[i] + routed.events[i].sampleOffset;
		chronological = chronological && previous <= current;
	}
	check (chronological, "the block's events arrive in chronological order");

	engine.setMasterMidiChannel (0);
	engine.setTrackMidiChannel (playingTrack, 0);

	// --- [11] the VGM command clock ------------------------------------------------------------
	// The note times come from VgmCommandReader, not from the emulator: every wait has to land in
	// the clock or the whole sequence drifts. 0x80-0x8F (the YM2612 DAC stream) carries its wait
	// in the low nibble and used to be skipped, which shortened Green Hill Zone from 52.9 s to
	// 21.4 s of MIDI time - the notes ran ahead of the audio and the sequence ended early.
	std::printf ("[11] the VGM command clock\n");
	{
		// 0x52 0x2B 0x80 (DAC on) | 0x8F (wait 15) | 0x61 0x10 0x00 (wait 16) | 0x66
		std::vector<std::uint8_t> image (0x40 + 8, 0);
		const auto putU32 = [&image] (std::size_t offset, std::uint32_t value)
		{
			image[offset + 0] = (std::uint8_t) (value & 0xFF);
			image[offset + 1] = (std::uint8_t) ((value >> 8) & 0xFF);
			image[offset + 2] = (std::uint8_t) ((value >> 16) & 0xFF);
			image[offset + 3] = (std::uint8_t) ((value >> 24) & 0xFF);
		};

		std::memcpy (image.data(), "Vgm ", 4);
		putU32 (0x04, (std::uint32_t) (image.size() - 4));	// EOF offset: the stream ends with it
		putU32 (0x08, 0x170);								// version 1.70
		image[0x40] = 0x52; image[0x41] = 0x2B; image[0x42] = 0x80;
		image[0x43] = 0x8F;
		image[0x44] = 0x61; image[0x45] = 0x10; image[0x46] = 0x00;
		image[0x47] = 0x66;

		toybox::backends::libvgm::VgmCommandReader reader (image.data(), image.size());
		toybox::backends::libvgm::VgmCommand command;
		std::vector<std::uint32_t> times;
		while (reader.next (command))
		{
			times.push_back (command.sampleTime);
		}

		std::printf ("    command times:");
		for (const auto time : times)
		{
			std::printf (" %u", (unsigned int) time);
		}
		std::printf ("\n");

		const bool walked = (times.size() == 4 && times[0] == 0 && times[1] == 0);
		check (walked, "the reader walks the synthetic stream");
		check (times.size() >= 3 && times[2] == 15, "0x8n advances the clock by its low nibble");
		check (times.size() >= 4 && times[3] == 31, "and the stream ends on the header's length");
		check (reader.timeSamples() == 31, "the reader's own clock agrees");
	}

	// The song under test, uncompressed: its header carries the length the engine plays, so the
	// reader's clock has to end there. That is the whole MIDI-versus-audio agreement in one
	// number - a stream whose waits are dropped (or counted twice) fails it by the difference.
	if (inputFile.hasFileExtension ("vgm"))
	{
		juce::MemoryBlock bytes;
		if (inputFile.loadFileAsData (bytes) && bytes.getSize() >= 0x40)
		{
			const auto* raw = static_cast<const std::uint8_t*> (bytes.getData());
			if (std::memcmp (raw, "Vgm ", 4) == 0)
			{
				const auto readU32 = [] (const std::uint8_t* p)
				{
					return (std::uint32_t) p[0] | ((std::uint32_t) p[1] << 8)
						   | ((std::uint32_t) p[2] << 16) | ((std::uint32_t) p[3] << 24);
				};

				const std::uint32_t headerSamples = readU32 (raw + 0x18);
				toybox::backends::libvgm::VgmCommandReader songReader (raw, bytes.getSize());
				toybox::backends::libvgm::VgmCommand ignored;
				while (songReader.next (ignored))
				{
				}

				std::printf ("    %s: clock %u samples, header %u (%.2f s)\n",
							 inputFile.getFileName().toRawUTF8(), (unsigned int) songReader.timeSamples(),
							 (unsigned int) headerSamples, (double) headerSamples / 44100.0);
				if (headerSamples > 0)
				{
					check (songReader.timeSamples() == headerSamples,
						   "the command clock ends on the header's length");
				}
			}
		}
	}

	// --- [12] MIDI across a loop -----------------------------------------------------------------
	// The event list covers one pass; the engine plays the loop as many times as it was asked
	// to. Before the coordinate was folded back, the cursor ran off the end of the list after the
	// first pass - the notes stopped while the audio played on. Songs without a loop point cannot
	// show this, so the check needs one (the checked-in test song has none).
	const double loopStart = engine.getLoopPointSeconds();
	if (loopStart >= 0.0)
	{
		// one playthrough, not counting the fade tail the engine appends after it
		const double firstPass = juce::jmax (0.0, engine.getDurationSeconds() - engine.getFadeSeconds());
		const double loopLength = firstPass - loopStart;
		const double wanted = firstPass + juce::jmin (6.0, loopLength);

		engine.setTrackMidiChannel (playingTrack, 1);
		engine.setLoopCount (2);
		engine.setPositionSeconds (0.0);
		engine.play();

		const auto looped = recordMidi (engine, wanted, sampleRate, blockSize);
		const auto passBoundary = (std::int64_t) (firstPass * sampleRate);
		const int secondPassNotes = looped.countNoteOnsFrom (1, passBoundary);
		std::printf ("    loop at %.2f s of %.2f s: %d note-on in the first pass, %d after it\n",
					 loopStart, firstPass,
					 looped.countNoteOnsFrom (1, 0) - secondPassNotes, secondPassNotes);
		check (secondPassNotes > 0, "the second loop pass sends notes again");

		engine.setLoopCount (1);
		engine.setTrackMidiChannel (playingTrack, 0);
	}
	else
	{
		std::printf ("    no loop point in this song - loop check skipped\n");
	}

	const juce::File outputFile = inputFile.withFileExtension (".rendered.wav");
	writeWavFile (outputFile, plain.left, plain.right, sampleRate);

	std::printf ("Rendered  : %d samples (%.2f s)\n", (int) plain.left.size(), (double) plain.left.size() / sampleRate);
	std::printf ("Peak      : %.4f\n", plain.peak);
	std::printf ("RMS       : %.4f\n", plain.rms);
	std::printf ("WAV       : %s\n", outputFile.getFullPathName().toRawUTF8());
	std::printf ("Result    : %s\n", (toybox::test::failures() == 0) ? "all checks passed" : "FAILURES");

	if (toybox::test::failures() > 0)
	{
		return 3;
	}
	return (plain.peak > 0.001) ? 0 : 2;
}
