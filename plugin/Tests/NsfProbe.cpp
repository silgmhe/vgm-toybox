/*
    The player, driven the way the backend will drive it.

    Prints the one number that decides whether the CPU, the bus and the trampoline are all correct
    at once: how many sound-register writes the driver made. Zero means the program never ran, and
    no amount of chip work will produce sound.
*/

#include "playback/WaveExport.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <utility>
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

	if (argc > 3 && std::string (argv[2]) == "--waves")
	{
		const std::string directory = argv[3];

		// Same forced chip as --chips: the corpus declares no expansion, and the point of this
		// mode is to exercise the export path.
		auto forced = file;
		forced.chipFlags |= (uint8_t) toybox::formats::nsf::NsfChip::n163;

		toybox::backends::nsf::NsfPlayer player (forced, 44100.0);
		player.start ();

		std::vector<float> l (1024, 0.0f), r (1024, 0.0f);
		for (int i = 0; i < 200; ++i) { player.render (l.data (), r.data (), 1024); }

		toybox::playback::ChipStateSnapshot snapshot;
		player.captureState (snapshot);

		int written = 0;

		for (std::size_t i = 0; i < snapshot.channels.size (); ++i)
		{
			const auto& channel = snapshot.channels[i];

			if (channel.wave.size () < 2)
			{
				continue;
			}

			const auto path = directory + "/wave" + std::to_string (i + 1) + ".wav";

			if (toybox::playback::writeCycleWav (channel, path))
			{
				++written;
				std::printf ("  %s  cycle=%d freq=%.1f Hz\n", path.c_str (),
							 (int) channel.wave.size (), (double) channel.frequency);
			}
		}

		std::printf ("wrote %d files\n", written);
		return 0;
	}

	if (argc > 2 && std::string (argv[2]) == "--chips")
	{
		// The corpus files declare no expansion, so a file without one gets the N163 forced on: this
		// mode exists to exercise the state path, and a player built without the chip has nothing to
		// report. A file that does declare one keeps it - the question here is what that chip says
		// about itself, not what a different chip would have said.
		auto forced = file;

		if (forced.chip () == toybox::formats::nsf::NsfChip::none)
		{
			forced.chipFlags |= (uint8_t) toybox::formats::nsf::NsfChip::n163;
		}

		toybox::backends::nsf::NsfPlayer player (forced, 44100.0);
		player.start ();

		std::vector<float> l (1024, 0.0f), r (1024, 0.0f);
		for (int i = 0; i < 200; ++i) { player.render (l.data (), r.data (), 1024); }

		toybox::playback::ChipStateSnapshot snapshot;
		const bool ok = player.captureState (snapshot);

		std::string chipNames;
		for (const auto& name : snapshot.chips)
		{
			if (! chipNames.empty ()) { chipNames += " / "; }
			chipNames += name;
		}

		std::printf ("chips valid=%d [%s] channels=%d\n", ok ? 1 : 0,
					 chipNames.c_str (), (int) snapshot.channels.size ());

		for (const auto& channel : snapshot.channels)
		{
			// The width only means anything for a pulse (waveKind 2): every other shape fills the
			// field with the value a centred line is drawn at, and printing it would read as a
			// real duty cycle. This column exists because a pulse drawn at twice its width looked
			// plausible on screen until the number was written down.
			const bool isPulse = channel.waveKind == 2;
			char duty[ 16 ] = "-";

			if (isPulse)
			{
				std::snprintf (duty, sizeof (duty), "%.4f", (double) channel.duty);
			}

			std::printf ("  %-8s note=%3d vol=%.3f wave=%d duty=%-6s active=%d\n",
						 channel.name.c_str (), channel.note, (double) channel.volume,
						 (int) channel.wave.size (), duty, channel.active ? 1 : 0);
		}

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
			std::fprintf (stderr, "PROG samples=%lld elapsed=%lldms writes=%llu ram=%llu gate=%llu/%d clk=%llu pc=%04X\n",
						  counted, (long long) ms, (unsigned long long) player.soundWrites (),
				(unsigned long long) player.ramWrites (),
				(unsigned long long) player.gateWrites (),
				player.gateValue (),
				(unsigned long long) player.executedClocks (),
				(unsigned) player.currentPC ());
			std::fflush (stderr);
			nextReport += 44100;
		}
	}

	const double rms = counted > 0 ? std::sqrt (sumSquares / (double) counted) : 0.0;

	{
		const auto& hist = player.pcHistogram ();
		std::vector<std::pair<uint32_t, int>> top;

		for (int page = 0; page < 256; ++page)
		{
			if (hist[(std::size_t) page] > 0) { top.push_back ({ hist[(std::size_t) page], page }); }
		}

		std::sort (top.begin (), top.end (), [] (const auto& a, const auto& b) { return a.first > b.first; });

		std::printf ("PC pages:");
		for (std::size_t i = 0; i < top.size () && i < 6; ++i)
		{
			std::printf (" %02X00=%u", top[i].second, top[i].first);
		}
		std::printf ("\\n");
	}

	std::printf ("nsf %s chip=0x%02X pal=%d writes=%llu seconds=%.1f peak=%.4f rms=%.4f\n",
				 argv[1], (unsigned) file.chipFlags, file.isPal () ? 1 : 0,
				 (unsigned long long) player.soundWrites (), (double) counted / rate, peak, rms);

	return 0;
}
