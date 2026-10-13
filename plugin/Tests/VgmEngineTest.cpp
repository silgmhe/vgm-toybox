/*
	Console test for the plugin's playback engine: renders a VGM/VGZ file offline and writes the
	result to a WAV file, so that the audio path can be checked without a DAW.

	Usage: vgm_engine_test <file.vgm|file.vgz> [seconds] [sampleRate]
	Build: cmake --build <build dir> --target vgm_engine_test
*/

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include "vgm/VgmEngine.h"

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

	std::printf ("[1] creating engine (sample rate %d, block size %d)\n", sampleRate, blockSize);
	vgmplay::VgmEngine engine;
	engine.prepare ((double) sampleRate, blockSize);
	std::printf ("[2] engine ready, loading file\n");

	juce::String errorMessage;
	if (! engine.loadFile (inputFile, errorMessage))
	{
		std::printf ("Loading failed: %s\n", errorMessage.toRawUTF8());
		return 1;
	}
	std::printf ("[3] file loaded\n");

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

	std::printf ("[4] configuring transport\n");
	engine.setLoopCount (1);
	std::printf ("[4a] loop count set\n");
	engine.setFadeSeconds (0.0);
	std::printf ("[4b] fade set\n");
	engine.play();
	std::printf ("[4c] playing\n");

	const int totalSamples = (int) (seconds * sampleRate);
	std::vector<float> left, right;
	left.reserve ((size_t) totalSamples);
	right.reserve ((size_t) totalSamples);

	std::vector<float> blockL ((size_t) blockSize), blockR ((size_t) blockSize);
	float* outputs[2] = { blockL.data(), blockR.data() };

	double peak = 0.0;
	double sumOfSquares = 0.0;
	size_t sampleCount = 0;

	int blockIndex = 0;
	while ((int) left.size() < totalSamples && ! engine.isFinished())
	{
		std::fill (blockL.begin(), blockL.end(), 0.0f);
		std::fill (blockR.begin(), blockR.end(), 0.0f);
		if (blockIndex < 3)
		{
			std::printf ("[5] rendering block %d\n", blockIndex);
		}
		engine.render (outputs, blockSize);
		if (blockIndex < 3)
		{
			std::printf ("[5] block %d done (L[0]=%f, L[100]=%f)\n", blockIndex, blockL[0], blockL[100]);
		}
		++blockIndex;

		for (int i = 0; i < blockSize && (int) left.size() < totalSamples; i ++)
		{
			left.push_back (blockL[(size_t) i]);
			right.push_back (blockR[(size_t) i]);
			peak = juce::jmax (peak, (double) std::abs (blockL[(size_t) i]));
			sumOfSquares += (double) blockL[(size_t) i] * blockL[(size_t) i];
			++sampleCount;
		}
	}

	const double durationBefore = engine.getDurationSeconds();
	std::printf ("[6] re-preparing at 96 kHz (like a host does after restoring a session)\n");
	engine.prepare (96000.0, blockSize);
	std::printf ("    duration = %.2f s (was %.2f s), position = %.2f s\n",
				 engine.getDurationSeconds(), durationBefore, engine.getPositionSeconds());

	// --- loop forever + seek ------------------------------------------------------------------
	// With an infinite loop libvgm reports a negative total time ("infinite runtime"), which must
	// not leak into the seek bar: the duration would be 0 and every seek would be clamped to 0.
	std::printf ("[7] loop / seek checks\n");
	engine.setLoopCount (1);
	const double oneLoopLength = engine.getDurationSeconds();
	engine.setLoopCount (2);
	const double twoLoopLength = engine.getDurationSeconds();
	engine.setLoopCount (0);
	const double foreverLength = engine.getDurationSeconds();
	if (oneLoopLength <= 0.0 || foreverLength <= 0.0)
	{
		std::printf ("    FAIL: duration is %.2f s with 1 loop, %.2f s while looping forever\n",
					 oneLoopLength, foreverLength);
		return 3;
	}

	engine.setPositionSeconds (foreverLength * 0.5);
	const double seekedPosition = engine.getPositionSeconds();
	std::printf ("    duration: %.2f s (1 loop) / %.2f s (2 loops) / %.2f s (forever)\n",
				 oneLoopLength, twoLoopLength, foreverLength);
	std::printf ("    seek to %.2f s -> %.2f s\n", foreverLength * 0.5, seekedPosition);

	// the loop point is what the editor marks above the position slider; a file that "plays
	// twice as long with 2 loops" must also report one (the 2-loop time proves it has a loop point)
	const double loopPoint = engine.getLoopPointSeconds();
	if (twoLoopLength > oneLoopLength * 1.5)
	{
		if (loopPoint <= 0.0 || loopPoint >= foreverLength)
		{
			std::printf ("    FAIL: loop point is %.2f s (song length %.2f s)\n", loopPoint, foreverLength);
			return 3;
		}
		std::printf ("    loop point: %.2f s (%.0f%% of the song - where the seek bar jumps back to)\n",
					 loopPoint, 100.0 * loopPoint / foreverLength);
	}
	else
	{
		std::printf ("    loop point: none (the file has no loop)\n");
	}
	if (seekedPosition < foreverLength * 0.25)
	{
		std::printf ("    FAIL: the seek did not take effect (position %.2f s)\n", seekedPosition);
		return 3;
	}

	const double rms = (sampleCount > 0) ? std::sqrt (sumOfSquares / (double) sampleCount) : 0.0;
	const juce::File outputFile = inputFile.withFileExtension (".rendered.wav");
	writeWavFile (outputFile, left, right, sampleRate);

	std::printf ("Rendered  : %d samples (%.2f s)\n", (int) left.size(), (double) left.size() / sampleRate);
	std::printf ("Position  : %.2f s / finished: %s\n", engine.getPositionSeconds(), engine.isFinished() ? "yes" : "no");
	std::printf ("Peak      : %.4f\n", peak);
	std::printf ("RMS       : %.4f\n", rms);
	std::printf ("WAV       : %s\n", outputFile.getFullPathName().toRawUTF8());

	return (peak > 0.001) ? 0 : 2;
}
