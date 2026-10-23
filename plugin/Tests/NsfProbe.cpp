// Console probe for the NSF player: a file in, a peak and an RMS out, and optionally a WAV.
//
// The chip test answers "did a register write become the right note"; this answers "does a driver
// that was written for a 6502 make sound at all", which is a different question with a different
// failure mode - a wrong memory map or a wrong frame rate produces silence or a drone rather than a
// wrong note.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "backends/nsf/NsfPlayer.h"

namespace
{

void writeWav (const char* path, const std::vector<std::int16_t>& samples)
{
	std::FILE* file = std::fopen (path, "wb");
	if (file == nullptr)
	{
		return;
	}

	const std::uint32_t dataBytes = (std::uint32_t) samples.size() * 2;
	const std::uint32_t riffSize = 36 + dataBytes;
	const std::uint32_t fmtSize = 16;
	const std::uint16_t format = 1;
	const std::uint16_t channels = 2;
	const std::uint32_t rate = 44100;
	const std::uint32_t byteRate = rate * 4;
	const std::uint16_t align = 4;
	const std::uint16_t bits = 16;

	std::fwrite ("RIFF", 1, 4, file);
	std::fwrite (&riffSize, 4, 1, file);
	std::fwrite ("WAVEfmt ", 1, 8, file);
	std::fwrite (&fmtSize, 4, 1, file);
	std::fwrite (&format, 2, 1, file);
	std::fwrite (&channels, 2, 1, file);
	std::fwrite (&rate, 4, 1, file);
	std::fwrite (&byteRate, 4, 1, file);
	std::fwrite (&align, 2, 1, file);
	std::fwrite (&bits, 2, 1, file);
	std::fwrite ("data", 1, 4, file);
	std::fwrite (&dataBytes, 4, 1, file);
	std::fwrite (samples.data(), 2, samples.size(), file);
	std::fclose (file);
}

} // namespace

int main (int argc, char** argv)
{
	if (argc < 3)
	{
		std::printf ("usage: vgm_nsf_probe <file.nsf> <seconds> [out.wav]\n");
		return 1;
	}

	std::ifstream file (argv[1], std::ios::binary);
	const std::vector<char> bytes ((std::istreambuf_iterator<char> (file)),
								   std::istreambuf_iterator<char>());

	if (bytes.empty())
	{
		std::printf ("error cannot read %s\n", argv[1]);
		return 1;
	}

	toybox::backends::nsf::NsfPlayer player (44100.0);
	std::string error;

	if (! player.load (bytes.data(), bytes.size(), error))
	{
		std::printf ("error %s\n", error.c_str());
		return 1;
	}

	const int seconds = std::atoi (argv[2]);
	const int total = 44100 * (seconds > 0 ? seconds : 5);

	constexpr int blockSize = 512;
	std::vector<float> left ((std::size_t) blockSize, 0.0f);
	std::vector<float> right ((std::size_t) blockSize, 0.0f);
	std::vector<std::int16_t> captured;
	captured.reserve ((std::size_t) total * 2);

	double peak = 0.0;
	double sumSquares = 0.0;
	std::int64_t counted = 0;

	for (int done = 0; done < total; done += blockSize)
	{
		const int chunk = std::min (blockSize, total - done);
		std::fill (left.begin(), left.end(), 0.0f);
		std::fill (right.begin(), right.end(), 0.0f);

		player.render (left.data(), right.data(), chunk);

		for (int i = 0; i < chunk; ++i)
		{
			const double sample = (double) left[(std::size_t) i];
			peak = std::max (peak, std::abs (sample));
			sumSquares += sample * sample;
			++counted;

			if (argc >= 4)
			{
				const double clamped = std::clamp (sample, -1.0, 1.0);
				const auto value = (std::int16_t) (clamped * 32000.0);
				captured.push_back (value);
				captured.push_back (value);
			}
		}
	}

	const double rms = counted > 0 ? std::sqrt (sumSquares / (double) counted) : 0.0;

	std::printf ("nsf %s chip=%d pal=%d writes=%llu seconds=%.1f peak=%.4f rms=%.4f\n", argv[1],
				 (int) player.chip(), player.isPal() ? 1 : 0, (unsigned long long) player.soundWrites(), (double) total / 44100.0, peak, rms);

	if (argc >= 4)
	{
		writeWav (argv[3], captured);
	}

	return 0;
}
