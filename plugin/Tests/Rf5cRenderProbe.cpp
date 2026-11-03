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
		// Sixty-four milliseconds of quiet ends a note. Three windows (16 ms) was too eager: a
		// percussive channel retriggered every 124 ms came out as one 491 ms note, and the export was
		// then blamed for being a quarter of its length.
		else if (w - last >= 12)
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

/** Loudness in `bins` equal slices, normalised so the shape - not the gain - is what is compared. */
std::vector<double> envelopeOf (const std::vector<float>& samples, int from, int count, int bins)
{
	std::vector<double> envelope ((size_t) bins, 0.0);
	double loudest = 0.0;

	for (int b = 0; b < bins; ++b)
	{
		const int begin = from + count * b / bins;
		const int end = from + count * (b + 1) / bins;

		double sum = 0.0;
		int n = 0;

		for (int i = begin; i < end && i < (int) samples.size (); ++i)
		{
			sum += (double) samples[(size_t) i] * samples[(size_t) i];
			++n;
		}

		envelope[(size_t) b] = (n > 0) ? std::sqrt (sum / n) : 0.0;
		loudest = juce::jmax (loudest, envelope[(size_t) b]);
	}

	if (loudest > 0.0)
	{
		for (auto& v : envelope)
		{
			v /= loudest;
		}
	}

	return envelope;
}

/** The same shape, from sign-magnitude bytes. */
std::vector<double> envelopeOfBytes (const uint8_t* bytes, int count, int bins)
{
	std::vector<double> envelope ((size_t) bins, 0.0);
	double loudest = 0.0;

	for (int b = 0; b < bins; ++b)
	{
		const int begin = count * b / bins;
		const int end = count * (b + 1) / bins;

		double sum = 0.0;
		int n = 0;

		for (int i = begin; i < end; ++i)
		{
			const double level = (double) (bytes[i] & 0x7F) / 128.0;
			sum += level * level;
			++n;
		}

		envelope[(size_t) b] = (n > 0) ? std::sqrt (sum / n) : 0.0;
		loudest = juce::jmax (loudest, envelope[(size_t) b]);
	}

	if (loudest > 0.0)
	{
		for (auto& v : envelope)
		{
			v /= loudest;
		}
	}

	return envelope;
}

/** Sign changes per second - the cheapest way to tell a tone from noise.

	A hi-hat is broadband: it crosses zero thousands of times a second and its waveform looks like
	noise at any rate. A piano or a bass crosses at roughly twice its fundamental and looks smooth.
	The number does not depend on the rate the bytes are read at in the way a waveform comparison
	does, which is why it can settle "is this slot the same instrument" without knowing the rate.
*/
double crossingRate (const std::vector<float>& samples, int from, int count)
{
	int crossings = 0;

	for (int i = from + 1; i < from + count && i < (int) samples.size (); ++i)
	{
		if ((samples[(size_t) i] >= 0.0f) != (samples[(size_t) (i - 1)] >= 0.0f))
		{
			++crossings;
		}
	}

	const double seconds = (double) count / (double) kSampleRate;

	return (seconds > 0.0) ? (double) crossings / seconds : 0.0;
}

/** The same for sign-magnitude bytes, at a nominal rate - the shape, not the speed. */
double crossingRateOfBytes (const uint8_t* bytes, int count)
{
	int crossings = 0;
	bool previous = (bytes[0] & 0x80) != 0;

	for (int i = 1; i < count; ++i)
	{
		const bool current = (bytes[i] & 0x80) != 0;

		if (current != previous)
		{
			++crossings;
		}

		previous = current;
	}

	// Per byte rather than per second: the rate is exactly what is in doubt, so the answer is
	// reported as "sign changes in N bytes" and compared with the render's own period.
	return (double) crossings / (double) count;
}

/** Pearson correlation of two envelopes: 1 is the same shape, 0 unrelated, -1 inverted. */
double shapeScore (const std::vector<double>& a, const std::vector<double>& b)
{
	const size_t n = juce::jmin (a.size (), b.size ());

	if (n == 0)
	{
		return 0.0;
	}

	double meanA = 0.0;
	double meanB = 0.0;

	for (size_t i = 0; i < n; ++i)
	{
		meanA += a[i];
		meanB += b[i];
	}

	meanA /= (double) n;
	meanB /= (double) n;

	double num = 0.0;
	double d1 = 0.0;
	double d2 = 0.0;

	for (size_t i = 0; i < n; ++i)
	{
		const double x = a[i] - meanA;
		const double y = b[i] - meanB;
		num += x * y;
		d1 += x * x;
		d2 += y * y;
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

			const std::vector<double> want = envelopeOf (audio, note.start, note.length, 32);
			const double seconds = (double) note.length / (double) kSampleRate;

			// **The shape, not the waveform.** A periodic signal decorrelates the moment the rate is
			// off by a percent, so comparing samples only ever compared rates. The loudness envelope
			// over the note is rate-blind: it says piano (attack, long decay) or drum (one burst) or
			// loop (repeats) without caring how fast the bytes were read. The length that wins also
			// gives the rate, because the note's duration is known: bytes / seconds.
			static const int kSpans[] = { 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192 };

			for (uint32_t slot = 0; slot + 0x2000 <= ram.size (); slot += 0x2000)
			{
				for (int span : kSpans)
				{
					const std::vector<double> have = envelopeOfBytes (ram.data () + slot, span, 32);
					const double score = shapeScore (want, have);

					matches.push_back ({ score, slot, (double) span / seconds });
				}
			}

			std::sort (matches.begin (), matches.end (),
					   [] (const Match& a, const Match& b) { return a.r > b.r; });

			// A tone crosses zero about twice per cycle; noise crosses far more often. Printing both
			// sides says whether the bank holds anything tonal at all - and if it does not, no
			// address mapping can explain a piano.
			std::printf ("     tone   : render %.0f/s", crossingRate (audio, note.start, note.length));

			// **One fixed rate for every slot**, so the column is comparable with the render. The
			// first version multiplied each slot by whatever rate the shape search had liked for it,
			// which made the numbers look like they measured the slot when they measured the guess.
			constexpr double kCommonRate = 10442.7;

			for (uint32_t slot = 0; slot + 0x2000 <= ram.size (); slot += 0x2000)
			{
				const double perByte = crossingRateOfBytes (ram.data () + slot, 0x2000);

				std::printf ("  0x%04X %.0f/s", (unsigned) slot, perByte * kCommonRate);
			}

			std::printf ("\n");

			std::printf ("     shape  :");

			for (int i = 0; i < 3 && i < (int) matches.size (); ++i)
			{
				std::printf ("  0x%04X@%.0fB/s r=%.2f", (unsigned) matches[(size_t) i].slot,
							 matches[(size_t) i].rate, matches[(size_t) i].r);
			}

			std::printf ("\n");

			if (! matches.empty () && matches[0].r > 0.9)
			{
				std::printf ("     verdict: MATCH 0x%04X at %.0f bytes/s (%.3f s)\n",
							 (unsigned) matches[0].slot, matches[0].rate, seconds);
			}
			else
			{
				std::printf ("     verdict: no slot matches this note\n");
			}
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
