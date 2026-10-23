// Console probe for libgme's NSF player.
//
// Its only job is to answer two questions with numbers: does a vendored libgme open our NSF files
// and make sound, and does what it makes agree with the reference player. Everything about the
// format - the header, the 6502 driver, the bank switching, the expansion chips - is libgme's.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

#include "Nsf_Emu.h"

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
		std::printf ("usage: vgm_gme_probe <file.nsf> <seconds> [out.wav]\n");
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

	Nsf_Emu emu;
	emu.set_sample_rate (44100);

	if (emu.load_mem (bytes.data(), (long) bytes.size()) != nullptr)
	{
		std::printf ("error libgme refused the file\n");
		return 1;
	}

	emu.start_track (0);

	const int seconds = std::atoi (argv[2]);
	const int total = 44100 * (seconds > 0 ? seconds : 5);

	std::vector<short> buffer ((std::size_t) 1024 * 2);
	std::vector<std::int16_t> captured;

	double peak = 0.0;
	double sumSquares = 0.0;
	std::int64_t counted = 0;

	for (int done = 0; done < total; done += 1024)
	{
		const int frames = std::min (1024, total - done);

		if (emu.play (frames, buffer.data()) != nullptr)
		{
			std::printf ("error play failed\n");
			return 1;
		}

		for (int i = 0; i < frames; ++i)
		{
			const double sample = (double) buffer[(std::size_t) i * 2] / 32768.0;
			peak = std::max (peak, std::abs (sample));
			sumSquares += sample * sample;
			++counted;

			if (argc >= 4)
			{
				captured.push_back (buffer[(std::size_t) i * 2]);
				captured.push_back (buffer[(std::size_t) i * 2 + 1]);
			}
		}
	}

	const double rms = counted > 0 ? std::sqrt (sumSquares / (double) counted) : 0.0;

	std::printf ("gme %s seconds=%.1f peak=%.4f rms=%.4f\n", argv[1],
				 (double) total / 44100.0, peak, rms);

	if (argc >= 4)
	{
		writeWav (argv[3], captured);
	}

	return 0;
}
