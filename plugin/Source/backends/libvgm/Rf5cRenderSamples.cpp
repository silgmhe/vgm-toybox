#include "Rf5c68Samples.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <utility>
#include <vector>

#include "LibvgmTrackPlayer.h"

namespace toybox::backends::libvgm
{
namespace
{
	/** The rate the channel is rendered at. The exporter writes at the project's rate anyway, and this
		is high enough that the chip's own 10-20 kHz content survives. */
	constexpr double kRenderRate = 44100.0;
	constexpr int kBlock = 4096;

	/** Twenty milliseconds: one envelope window. Short enough that a 40 ms burst is two windows, long
		enough that a single cycle of a low note does not read as silence. */
	constexpr std::size_t kWindow = (std::size_t) (kRenderRate / 50.0);

	/** Two quiet windows end a burst - 40 ms, longer than the gap inside a drum roll and shorter than
		the gap between two hits of one instrument in these rips. */
	constexpr std::size_t kSilenceWindows = 2;

	/** A window sounds when it is above this fraction of the channel's loudest window. Relative,
		because the chip's absolute scale is arbitrary: an absolute threshold either misses the quiet
		channels or reports the loud ones' noise floor. */
	constexpr double kLevelFraction = 0.25;

	/** Fewer windows than this is a click, not a hit. */
	constexpr std::size_t kMinimumWindows = 2;

	uint64_t hashOfRange (const std::vector<float>& samples, std::size_t first, std::size_t last)
	{
		// Hashed as the file is written - 16-bit - so two bursts that differ only below that
		// resolution are the same file.
		uint64_t hash = 1469598103934665603ull;

		for (std::size_t i = first; i < last; ++i)
		{
			const int16_t value = (int16_t) (samples[i] * 32767.0f);

			hash = (hash ^ (uint64_t) (value & 0xFF)) * 1099511628211ull;
			hash = (hash ^ (uint64_t) ((value >> 8) & 0xFF)) * 1099511628211ull;
		}

		return hash;
	}

	/** Renders one channel of the song on its own, at `kRenderRate`, mixed to mono. */
	std::vector<float> renderChannel (const playback::SongMemory& memory, int channel)
	{
		LibvgmTrackPlayer player;
		juce::String error;

		player.prepare (kRenderRate, kBlock);

		if (! player.load (memory, error))
		{
			return {};
		}

		// The mask is the whole point: measured on the chip itself (`none=3776, ch0-muted=0, all=0`),
		// so what comes back is this voice and nothing else.
		player.isolateRf5cChannel (channel);
		player.start ();

		std::vector<float> mono;
		std::vector<float> left ((std::size_t) kBlock);
		std::vector<float> right ((std::size_t) kBlock);

		while (! player.isFinished ())
		{
			const int written = player.render (left.data (), right.data (), kBlock);

			if (written <= 0)
			{
				break;
			}

			for (int i = 0; i < written; ++i)
			{
				mono.push_back (0.5f * (left[(std::size_t) i] + right[(std::size_t) i]));
			}
		}

		return mono;
	}
}

std::vector<playback::DecodedSample> renderRf5cSamples (const playback::SongMemory& memory)
{
	std::vector<playback::DecodedSample> done;
	std::map<uint64_t, bool> seen;

	for (int channel = 0; channel < 8; ++channel)
	{
		const std::vector<float> mono = renderChannel (memory, channel);

		if (mono.size () < kWindow * kMinimumWindows)
		{
			continue;
		}

		std::vector<double> levels;

		for (std::size_t start = 0; start + kWindow <= mono.size (); start += kWindow)
		{
			double sum = 0.0;

			for (std::size_t i = 0; i < kWindow; ++i)
			{
				const double value = mono[start + i];
				sum += value * value;
			}

			levels.push_back (std::sqrt (sum / (double) kWindow));
		}

		double loudest = 0.0;

		for (const double level : levels)
		{
			loudest = std::max (loudest, level);
		}

		if (loudest <= 0.0)
		{
			continue;			// a channel this song never uses
		}

		const double floorLevel = loudest * kLevelFraction;

		std::size_t begin = 0;
		std::size_t silence = 0;
		bool inBurst = false;
		int index = 0;

		for (std::size_t i = 0; i <= levels.size (); ++i)
		{
			const bool sounding = (i < levels.size ()) && (levels[i] > floorLevel);

			if (sounding)
			{
				if (! inBurst)
				{
					inBurst = true;
					begin = i;
				}

				silence = 0;
				continue;
			}

			if (! inBurst)
			{
				continue;
			}

			if (++silence <= kSilenceWindows)
			{
				continue;		// the tail of the burst, not the end of it
			}

			inBurst = false;

			const std::size_t first = begin * kWindow;
			const std::size_t last =
				(std::min) ((i - silence + 1) * kWindow, mono.size ());

			if ((i - silence + 1) <= begin + kMinimumWindows)
			{
				continue;
			}

			const uint64_t hash = hashOfRange (mono, first, last);

			if (! seen.insert (std::make_pair (hash, true)).second)
			{
				continue;		// the same hit again
			}

			char name[48] = {};
			std::snprintf (name, sizeof (name), "rf5c_ch%d_%02d_%04X", channel, index++,
						   (unsigned) (hash & 0xFFFFu));

			playback::DecodedSample sample;
			sample.name = name;
			sample.rate = kRenderRate;
			sample.values.assign (mono.begin () + (std::ptrdiff_t) first,
								  mono.begin () + (std::ptrdiff_t) last);

			done.push_back (std::move (sample));
		}
	}

	return done;
}

} // namespace toybox::backends::libvgm
