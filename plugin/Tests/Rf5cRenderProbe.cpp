#include <juce_core/juce_core.h>

#include "playback/ChipState.h"
#include "playback/PlaybackEngine.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

/**
 * Renders one channel of a song and compares it, note by note, with the sample the display
 * believes that channel plays.
 *
 * The point of this program is that it is **not** a unit test. Everything else in this branch
 * argues from the register stream: which `0x06` write set the start, which `0x08` edge opened the
 * gate, what rate the `0x02`/`0x03` pair asks for. Those arguments have been wrong about this chip
 * three times. This one asks the emulator instead: solo a channel, render it, and measure what
 * actually came out - then measure the wave the drag export would write and put the two numbers
 * next to each other. If they disagree, the disagreement is a fact, not an inference.
 *
 *   build <dir>/plugin/vgm_rf5c_probe <song.vgm|vgz> [trackIndex] [seconds]
 *
 * `trackIndex` of -1 (the default) walks every track.
 */

namespace
{
constexpr int kSampleRate = 48000;
constexpr int kBlockSize = 512;

/** One stretched stretch of audio above the noise floor. */
struct NoteWindow
{
	int start = 0;			// first sample
	int length = 0;			// samples
	double peak = 0.0;
	double pitch = 0.0;		// Hz, 0 when the autocorrelation found nothing usable
};

/** `seconds` of the engine's stereo output, left channel only. */
std::vector<float> renderSeconds (toybox::PlaybackEngine& engine, double seconds)
{
	std::vector<float> out;
	out.reserve ((size_t) (seconds * kSampleRate));

	std::vector<float> blockL ((size_t) kBlockSize), blockR ((size_t) kBlockSize);
	toybox::playback::OutputBusBuffers buses[1] = { { blockL.data(), blockR.data() } };

	const size_t total = (size_t) (seconds * kSampleRate);

	while (out.size () < total && ! engine.isFinished ())
	{
		std::fill (blockL.begin (), blockL.end (), 0.0f);
		std::fill (blockR.begin (), blockR.end (), 0.0f);
		engine.render (buses, 1, kBlockSize);

		for (int i = 0; i < kBlockSize && out.size () < total; ++i)
		{
			out.push_back (blockL[(size_t) i]);
		}
	}

	return out;
}

/** Normalised autocorrelation over a musical range of lags; returns the strongest period in Hz. */
double pitchOf (const std::vector<float>& samples, int from, int length, double minHz, double maxHz)
{
	const int minLag = (int) std::floor ((double) kSampleRate / maxHz);
	const int maxLag = (int) std::floor ((double) kSampleRate / minHz);
	const int span = juce::jmin (length, 8192);

	if (span < minLag * 4)
	{
		return 0.0;
	}

	double best = -2.0;
	int bestLag = 0;

	for (int lag = minLag; lag <= maxLag && lag < span / 2; ++lag)
	{
		double num = 0.0;
		double d1 = 0.0;
		double d2 = 0.0;

		for (int i = 0; i < span - lag; ++i)
		{
			const double a = samples[(size_t) (from + i)];
			const double b = samples[(size_t) (from + i + lag)];
			num += a * b;
			d1 += a * a;
			d2 += b * b;
		}

		if (d1 > 0.0 && d2 > 0.0)
		{
			const double r = num / std::sqrt (d1 * d2);

			if (r > best)
			{
				best = r;
				bestLag = lag;
			}
		}
	}

	return (bestLag > 0) ? (double) kSampleRate / bestLag : 0.0;
}

/** The first stretch that rises above the floor and then falls back under it. */
NoteWindow firstNote (const std::vector<float>& samples)
{
	NoteWindow window;

	constexpr int kWindow = 256;
	const int windows = (int) samples.size () / kWindow;

	std::vector<double> energy ((size_t) windows, 0.0);
	double loudest = 0.0;

	for (int w = 0; w < windows; ++w)
	{
		double sum = 0.0;

		for (int i = 0; i < kWindow; ++i)
		{
			const double v = samples[(size_t) (w * kWindow + i)];
			sum += v * v;
		}

		energy[(size_t) w] = std::sqrt (sum / kWindow);
		loudest = juce::jmax (loudest, energy[(size_t) w]);
	}

	if (loudest <= 0.0)
	{
		return window;
	}

	const double floorLevel = loudest * 0.02;
	int first = -1;

	for (int w = 0; w < windows; ++w)
	{
		if (energy[(size_t) w] > floorLevel)
		{
			first = w;
			break;
		}
	}

	if (first < 0)
	{
		return window;
	}

	int last = first;

	for (int w = first; w < windows; ++w)
	{
		if (energy[(size_t) w] > floorLevel)
		{
			last = w;
		}
		else if (w - last >= 3)
		{
			break;
		}
	}

	window.start = first * kWindow;
	window.length = (last - first + 1) * kWindow;
	window.peak = loudest;
	window.pitch = pitchOf (samples, window.start, window.length, 30.0, 3000.0);

	return window;
}

/** The wave the drag export would write: period in samples, and the pitch that implies. */
struct ExportShape
{
	bool present = false;
	size_t length = 0;
	double rate = 0.0;
	int period = 0;
	double impliedPitch = 0.0;
};

ExportShape shapeOf (const toybox::playback::ChannelState& channel)
{
	ExportShape shape;

	if (channel.wave == nullptr || channel.wave->size () < 8)
	{
		return shape;
	}

	shape.present = true;
	shape.length = channel.wave->size ();
	shape.rate = (double) channel.sampleRate;

	// The wave's own period, in its own samples, by autocorrelation.
	double best = -2.0;

	for (int lag = 2; lag < (int) shape.length / 2; ++lag)
	{
		double num = 0.0;
		double d1 = 0.0;
		double d2 = 0.0;

		for (size_t i = 0; i + (size_t) lag < shape.length; ++i)
		{
			const double a = (double) (*channel.wave)[i];
			const double b = (double) (*channel.wave)[i + (size_t) lag];
			num += a * b;
			d1 += a * a;
			d2 += b * b;
		}

		if (d1 > 0.0 && d2 > 0.0)
		{
			const double r = num / std::sqrt (d1 * d2);

			if (r > best)
			{
				best = r;
				shape.period = lag;
			}
		}
	}

	if (shape.period > 0 && shape.rate > 0.0)
	{
		shape.impliedPitch = shape.rate / (double) shape.period;
	}

	return shape;
}
} // namespace

int main (int argc, char** argv)
{
	if (argc < 2)
	{
		std::printf ("usage: %s <song.vgm|vgz> [trackIndex] [seconds]\n", argv[0]);
		return 1;
	}

	toybox::PlaybackEngine engine;
	engine.prepare ((double) kSampleRate, kBlockSize);

	juce::String error;

	if (! engine.loadFile (juce::File (juce::String (argv[1])), error))
	{
		std::printf ("load failed: %s\n", error.toRawUTF8 ());
		return 1;
	}

	// Mixer mode, or every solo renders the plain player and the rows never publish state.
	engine.setMixerEnabled (true);

	const auto& tracks = engine.getTracks ();
	const int wanted = (argc > 2) ? std::atoi (argv[2]) : -1;
	const double seconds = (argc > 3) ? std::atof (argv[3]) : 4.0;

	std::printf ("song: %s   tracks: %d   rendered: %.1f s\n", argv[1], (int) tracks.size (), seconds);

	for (size_t track = 0; track < tracks.size (); ++track)
	{
		if (wanted >= 0 && (int) track != wanted)
		{
			continue;
		}

		for (size_t other = 0; other < tracks.size (); ++other)
		{
			engine.setTrackSolo (other, other == track);
		}

		engine.setPositionSeconds (0.0);
		engine.play ();

		const std::vector<float> audio = renderSeconds (engine, seconds);

		// Before stop(), which tears the players down and takes every row's state with them.
		toybox::playback::ChipStateSnapshot snapshot;
		engine.captureChipState (snapshot);

		engine.stop ();

		for (size_t other = 0; other < tracks.size (); ++other)
		{
			engine.setTrackSolo (other, false);
		}

		const NoteWindow note = firstNote (audio);

		const toybox::playback::ChannelState* channel = nullptr;

		for (size_t i = 0; i < snapshot.channels.size (); ++i)
		{
			if ((int) i == (int) track)
			{
				channel = &snapshot.channels[i];
			}
		}

		std::printf ("\n[%2d] %-20s\n", (int) track, tracks[track].name.toRawUTF8 ());
		std::printf ("     state  : %d channels, %d chips\n",
					 (int) snapshot.channels.size (), (int) snapshot.chips.size ());

		if (note.length <= 0)
		{
			std::printf ("     render : silence\n");
		}
		else
		{
			std::printf ("     render : note at %.3f s, %.3f s long, peak %.3f, pitch %.1f Hz\n",
						 (double) note.start / kSampleRate, (double) note.length / kSampleRate,
						 note.peak, note.pitch);
		}

		if (channel == nullptr)
		{
			std::printf ("     export : no state\n");
			continue;
		}

		const ExportShape shape = shapeOf (*channel);

		if (! shape.present)
		{
			std::printf ("     export : no wave\n");
			continue;
		}

		std::printf ("     export : %zu samples at %.1f Hz = %.3f s, period %d -> %.1f Hz\n",
					 shape.length, shape.rate, (double) shape.length / shape.rate,
					 shape.period, shape.impliedPitch);

		if (note.pitch > 0.0 && shape.impliedPitch > 0.0)
		{
			std::printf ("     ratio  : export pitch / rendered pitch = %.3f\n",
						 shape.impliedPitch / note.pitch);
		}

		if (note.length > 0 && shape.rate > 0.0)
		{
			const double renderedSeconds = (double) note.length / kSampleRate;
			const double exportSeconds = (double) shape.length / shape.rate;
			std::printf ("     length : render %.3f s vs export %.3f s (ratio %.2f)\n",
						 renderedSeconds, exportSeconds, exportSeconds / renderedSeconds);
		}
	}

	std::printf ("\n");
	return 0;
}
