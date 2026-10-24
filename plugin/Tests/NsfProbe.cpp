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
			std::fprintf (stderr, "PROG samples=%lld elapsed=%lldms writes=%llu\n",
						  counted, (long long) ms, (unsigned long long) player.soundWrites ());
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
