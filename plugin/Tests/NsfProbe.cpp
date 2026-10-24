/*
    The player, driven the way the backend will drive it.

    Prints the one number that decides whether the CPU, the bus and the trampoline are all correct
    at once: how many sound-register writes the driver made. Zero means the program never ran, and
    no amount of chip work will produce sound.
*/

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

#include "backends/nsf/NsfPlayer.h"
#include "formats/nsf/NsfFile.h"

int main (int argc, char** argv)
{
	if (argc < 2)
	{
		std::printf ("usage: vgm_nsf_probe <file.nsf> [seconds]\n");
		return 1;
	}

	std::ifstream input (argv[1], std::ios::binary);
	const std::vector<char> bytes ((std::istreambuf_iterator<char> (input)),
								   std::istreambuf_iterator<char> ());

	if (bytes.empty ())
	{
		std::printf ("cannot read %s\n", argv[1]);
		return 1;
	}

	toybox::formats::nsf::NsfFile file;
	std::string error;

	if (! toybox::formats::nsf::NsfFile::parse (bytes.data (), bytes.size (), file, error))
	{
		std::printf ("not an NSF: %s\n", error.c_str ());
		return 1;
	}

	if (argc > 2 && std::string (argv[2]) == "--check")
	{
		// The channels must add up to what the mixed render produces. Run twice from the same
		// starting state: once mixed, once split, and compare. This is the end-to-end check the
		// per-channel path needs, and it is the only place the DMC's own path gets exercised with
		// the player's real wiring.
		const int frames = 4096;
		std::vector<float> left ((std::size_t) frames), right ((std::size_t) frames);
		std::vector<float> sumLeft ((std::size_t) frames, 0.0f), sumRight ((std::size_t) frames, 0.0f);

		{
			toybox::backends::nsf::NsfPlayer player (file, 44100.0);
			player.start ();
			player.render (left.data (), right.data (), frames);
		}

		{
			toybox::backends::nsf::NsfPlayer player (file, 44100.0);
			player.start ();

			const int channels = player.channelCount ();
			std::vector<std::vector<float>> buffers ((std::size_t) channels,
													 std::vector<float> ((std::size_t) frames * 2, 0.0f));
			std::vector<float*> pointers ((std::size_t) channels);

			for (int c = 0; c < channels; ++c)
			{
				pointers[(std::size_t) c] = buffers[(std::size_t) c].data ();
			}

			player.renderChannels (pointers.data (), channels, frames);

			for (int c = 0; c < channels; ++c)
			{
				float peak = 0.0f;
				for (float v : buffers[(std::size_t) c]) { peak = (std::max) (peak, std::fabs (v)); }
				std::printf ("CHANNELPEAK %d %.4f\n", c, (double) peak);
			}

			for (int c = 0; c < channels; ++c)
			{
				for (int i = 0; i < frames; ++i)
				{
					sumLeft[(std::size_t) i] += buffers[(std::size_t) c][(std::size_t) i * 2 + 0];
					sumRight[(std::size_t) i] += buffers[(std::size_t) c][(std::size_t) i * 2 + 1];
				}
			}
		}

		double worst = 0.0;

		for (int i = 0; i < frames; ++i)
		{
			worst = (std::max) (worst, (double) std::fabs (sumLeft[(std::size_t) i] - left[(std::size_t) i]));
			worst = (std::max) (worst, (double) std::fabs (sumRight[(std::size_t) i] - right[(std::size_t) i]));
		}

		std::printf ("channels sum vs mixed: worst difference %.6f over %d frames\n", worst, frames);
		// The mixed render clamps to +/-1; the channels are summed without clamping, so a loud moment
		// differs by exactly the amount that was clipped. That is the only expected difference.
		return worst < 0.05 ? 0 : 1;
	}

	if (argc > 3 && std::string (argv[2]) == "--seek")
	{
		const double target = std::atof (argv[3]);
		toybox::backends::nsf::NsfPlayer player (file, 44100.0);
		player.start ();

		const bool ok = player.seekTo (target);
		std::printf ("seek %s to %.2f -> position %.2f s\n", ok ? "ok" : "failed", target,
					 player.positionSeconds ());

		std::vector<float> l (1024, 0.0f), r (1024, 0.0f);
		double peak = 0.0;
		for (int i = 0; i < 20; ++i)
		{
			player.render (l.data (), r.data (), 1024);
			for (float v : l) { peak = (std::max) (peak, (double) std::fabs (v)); }
		}
		std::printf ("after seek peak %.4f\n", peak);
		return 0;
	}

	const double seconds = argc > 2 ? std::atof (argv[2]) : 5.0;
	const double rate = 44100.0;

	toybox::backends::nsf::NsfPlayer player (file, rate);
	player.start ();

	std::vector<float> left (1024, 0.0f);
	std::vector<float> right (1024, 0.0f);

	double peak = 0.0;
	double sumSquares = 0.0;
	long long counted = 0;
	const auto total = (long long) (rate * seconds);

	const auto started = std::chrono::steady_clock::now();
	auto nextReport = 44100LL;

	while (counted < total)
	{
		const int frames = player.render (left.data (), right.data (), 1024);

		for (int i = 0; i < frames; ++i)
		{
			const double value = (double) left[(std::size_t) i];
			peak = (std::max) (peak, (std::fabs) (value));
			sumSquares += value * value;
		}

		counted += frames;

		if (counted >= nextReport)
		{
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started).count();
			std::fprintf (stderr, "PROG samples=%lld elapsed=%lldms writes=%llu ram=%llu pc=%04X\n",
						  counted, (long long) ms, (unsigned long long) player.soundWrites (),
				(unsigned long long) player.ramWrites (),
				(unsigned) player.currentPC ());
			std::fflush (stderr);
			nextReport += 44100;
		}
	}

	const double rms = counted > 0 ? std::sqrt (sumSquares / (double) counted) : 0.0;

	std::printf ("nsf %s chip=0x%02X pal=%d writes=%llu seconds=%.1f peak=%.4f rms=%.4f\n",
				 argv[1], (unsigned) file.chipFlags, file.isPal () ? 1 : 0,
				 (unsigned long long) player.soundWrites (), (double) counted / rate, peak, rms);

	return 0;
}
