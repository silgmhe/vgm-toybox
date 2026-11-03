#include <juce_core/juce_core.h>

#include "backends/libvgm/LibvgmVgz.h"
#include "backends/libvgm/VgmCommandReader.h"
#include "playback/ChipState.h"
#include "playback/PlaybackEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

/** The sixty-four kilobytes the chip addresses, from the `0x02` blocks of the file. */
std::vector<uint8_t> ramImage (const juce::File& file)
{
	juce::MemoryBlock raw;

	if (! file.loadFileAsData (raw))
	{
		return {};
	}

	// The plugin's own inflater: the probe must not grow a dependency the plugin lacks.
	juce::MemoryBlock plain;

	if (! toybox::backends::libvgm::flattenCompressedImage (raw.getData (), raw.getSize (), plain))
	{
		return {};
	}

	const auto* begin = static_cast<const uint8_t*> (plain.getData());
	std::vector<uint8_t> image (begin, begin + plain.getSize ());

	if (image.size () < 0x40)
	{
		return {};
	}

	toybox::backends::libvgm::VgmCommandReader reader (image.data (), image.size ());
	toybox::backends::libvgm::VgmCommand command;

	while (reader.next (command))
	{
	}

	// The bank, positionally: every `0x02` block appended, exactly as `memory::pcmBank` does.
	std::vector<uint8_t> ram (64 * 1024, 0xFF);
	size_t at = 0;

	for (const auto& block : reader.dataBlocks())
	{
		if (block.type != 0x02)
		{
			continue;
		}

		const size_t count = juce::jmin (block.size, ram.size () - at);

		if (count == 0)
		{
			break;
		}

		std::memcpy (ram.data () + at, block.data, count);
		at += count;
	}

	return ram;
}

/** How much of `slot` played at `byteRate` looks like the rendered window. */
double correlate (const std::vector<float>& rendered, int start, int length,
				  const uint8_t* slot, int available, double byteRate)
{
	const double bytesPerSample = byteRate / (double) kSampleRate;
	const int span = juce::jmin (length, 8192);

	double num = 0.0;
	double d1 = 0.0;
	double d2 = 0.0;

	for (int i = 0; i < span; i += 4)
	{
		const size_t index = (size_t) ((double) i * bytesPerSample);

		if (index >= (size_t) available)
		{
			break;
		}

		const uint8_t byte = slot[index];
		const double level = (byte & 0x80) ? (double) (byte & 0x7F) / 128.0
										   : -(double) (byte & 0x7F) / 128.0;
		const double sample = rendered[(size_t) (start + i)];

		num += sample * level;
		d1 += sample * sample;
		d2 += level * level;
	}

	return (d1 > 0.0 && d2 > 0.0) ? num / std::sqrt (d1 * d2) : 0.0;
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
	const std::vector<uint8_t> ram = ramImage (juce::File (juce::String (argv[1])));

	std::printf ("ram    : %d bytes\n", (int) ram.size ());

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

		if (note.length > 0 && ! ram.empty ())
		{
			// **Which slot, at which rate, is the sound that just came out of the speaker.**
			// The register arguments have been wrong three times; this asks the bytes directly.
			//
			// **Known limitation, and it makes the numbers below useless as a verdict**: a periodic
			// signal decorrelates completely once the rate is off by more than about one percent over
			// the window (thirty-six cycles of a 71 Hz tone in half a second), and this grid steps in
			// tens of percent. Every r here is therefore near zero whether or not the bytes are right.
			// What it does show is which slots are not obviously unrelated; a rate has to be found by
			// measuring a period, not by searching a grid this coarse.
			static const double kRates[] = { 2000, 3000, 4000, 5000, 6500, 8000, 10000, 12500,
											 15000, 18000, 22000, 26000, 30000, 32552 };

			struct Match { double r; uint32_t slot; double rate; };
			std::vector<Match> matches;

			for (uint32_t slot = 0; slot + 0x2000 <= ram.size (); slot += 0x2000)
			{
				for (double rate : kRates)
				{
					matches.push_back ({ correlate (audio, note.start, note.length,
													ram.data () + slot, 0x2000, rate),
										 slot, rate });
				}
			}

			std::sort (matches.begin (), matches.end (),
					   [] (const Match& a, const Match& b) { return a.r > b.r; });

			std::printf ("     best   :");

			for (int i = 0; i < 3 && i < (int) matches.size (); ++i)
			{
				std::printf ("  0x%04X@%.0fHz r=%.2f", (unsigned) matches[(size_t) i].slot,
							 matches[(size_t) i].rate, matches[(size_t) i].r);
			}

			std::printf ("\n");
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
