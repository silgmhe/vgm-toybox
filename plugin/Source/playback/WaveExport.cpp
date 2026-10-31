#include "WaveExport.h"

#include "WaveShape.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

namespace toybox::playback
{
namespace
{
	void put32 (std::vector<uint8_t>& out, uint32_t value)
	{
		out.push_back ((uint8_t) (value & 0xFF));
		out.push_back ((uint8_t) ((value >> 8) & 0xFF));
		out.push_back ((uint8_t) ((value >> 16) & 0xFF));
		out.push_back ((uint8_t) ((value >> 24) & 0xFF));
	}

	void put16 (std::vector<uint8_t>& out, uint16_t value)
	{
		out.push_back ((uint8_t) (value & 0xFF));
		out.push_back ((uint8_t) ((value >> 8) & 0xFF));
	}

	/** `cycle` stretched to `samples` by repeating each source sample.

		Nearest-neighbour rather than linear, and that is the whole point: the chip quantises, the
		panel draws those quantisation steps, and a wavetable is meant to be the shape that was
		seen. Interpolation has been tried here twice - once from the start, and once only above
		eight bits - and both times it turned a thirty-two-step staircase into a smooth ramp, which
		is a shape the hardware never produced. Depth belongs to the vertical axis; holding a sample
		belongs to the horizontal one, and neither decides the other.

		Integer division keeps this exact for every length that divides 2048, and still lands on a
		step boundary for the ones that do not.
	*/
	std::vector<float> nearest (const std::vector<float>& cycle, std::size_t samples)
	{
		std::vector<float> out;
		out.reserve (samples);

		const auto size = cycle.size ();

		for (std::size_t i = 0; i < samples; ++i)
		{
			out.push_back (cycle[(i * size) / samples]);
		}

		return out;
	}

	/** `cycle` repeated `periods` times, unchanged - a loop for a sampler, not a shape. */
	std::vector<float> repeated (const std::vector<float>& cycle, int periods)
	{
		std::vector<float> out;
		out.reserve (cycle.size () * (std::size_t) std::max (1, periods));

		for (int period = 0; period < periods; ++period)
		{
			out.insert (out.end (), cycle.begin (), cycle.end ());
		}

		return out;
	}
}

std::vector<float> cycleOf (const ChannelState& channel)
{
	if (channel.wave.size () >= 2)
	{
		return channel.wave;
	}

	return waveform::cycleFor (channel.waveKind, channel.duty, channel.shapeParameter);
}

bool hasPeriodicShape (const ChannelState& channel)
{
	return channel.waveKind != (int) waveform::Kind::delta
		   && cycleOf (channel).size () >= 2;
}

int nativeBits (int waveKind)
{
	switch ((waveform::Kind) waveKind)
	{
	case waveform::Kind::pulse:		return 1;	// two levels, and no more
	case waveform::Kind::noise:		return 1;
	case waveform::Kind::triangle:	return 4;	// the counter runs to thirty-two, so sixteen steps
	case waveform::Kind::saw:		return 4;	// the VRC6's ramp is a four-bit counter
	case waveform::Kind::stored:	return 6;	// FDS is six bits, N163 four - six loses nothing
	case waveform::Kind::delta:		return 16;	// a mark, not a sample; nothing to snap
	case waveform::Kind::fm:		return 16;	// an analogue sum, which has no steps at all
	default:						return 16;
	}
}

namespace
{
	/** A RIFF file of thirty-two-bit floats.

		`marker` is written as the `clm ` chunk when it is not empty, and it is the only thing that
		separates the two destinations: a wavetable carries it so a synth can read the file as one
		cycle, and a sample carries none so the same synth reads it as audio.

		Four bytes per sample, because these are floats. The first version wrote sixteen-bit PCM,
		which is a legal wav and the wrong container: a reference wavetable from a wavetable synth
		is float32 - its fmt chunk carries format tag 3 - and quantising to seventeen levels only
		reads as steps if the values are the levels. Rounded into an integer scale they are the same
		shape with a different ruler, and the synth rescales what it is handed.
	*/
	bool writeFloat32Wav (const std::string& path, const std::vector<float>& samples,
						  uint32_t sampleRate, const std::string& marker)
	{
		const auto dataBytes = (uint32_t) samples.size () * 4u;

		std::vector<uint8_t> fmtChunk;
		{
			put32 (fmtChunk, 16u);				// chunk size
			put16 (fmtChunk, 3u);				// IEEE float
			put16 (fmtChunk, 1u);				// mono
			put32 (fmtChunk, sampleRate);
			put32 (fmtChunk, sampleRate * 4u);	// byte rate
			put16 (fmtChunk, 4u);				// block align
			put16 (fmtChunk, 32u);				// bits per sample
		}

		std::vector<uint8_t> clmChunk;

		if (! marker.empty ())
		{
			put32 (clmChunk, (uint32_t) marker.size ());
			clmChunk.insert (clmChunk.end (), marker.begin (), marker.end ());
		}

		std::vector<uint8_t> header;
		header.reserve (44 + clmChunk.size ());

		// "WAVE", then each chunk's eight-byte header plus its body: fmt always, clm only when
		// there is a marker, data always.
		const auto bodyBytes = 4u
							   + 8u + (uint32_t) fmtChunk.size ()
							   + (clmChunk.empty () ? 0u : 8u + (uint32_t) clmChunk.size ())
							   + 8u + dataBytes;

		header.insert (header.end (), { 'R', 'I', 'F', 'F' });
		put32 (header, bodyBytes);
		header.insert (header.end (), { 'W', 'A', 'V', 'E' });

		header.insert (header.end (), { 'f', 'm', 't', ' ' });
		header.insert (header.end (), fmtChunk.begin (), fmtChunk.end ());

		if (! clmChunk.empty ())
		{
			header.insert (header.end (), { 'c', 'l', 'm', ' ' });
			header.insert (header.end (), clmChunk.begin (), clmChunk.end ());
		}

		header.insert (header.end (), { 'd', 'a', 't', 'a' });
		put32 (header, dataBytes);

		std::ofstream file (path, std::ios::binary | std::ios::trunc);

		if (! file)
		{
			return false;
		}

		std::vector<uint8_t> fileBytes = header;
		fileBytes.reserve (header.size () + dataBytes);

		for (const float sample : samples)
		{
			// The value goes straight out: a float wav has no scale to lose the levels on, and the
			// reference file's levels sit exactly on -1, -0.875, ... 1.0.
			uint32_t bits = 0;
			static_assert (sizeof (float) == sizeof (uint32_t), "a float is four bytes here");

			std::memcpy (&bits, &sample, sizeof (bits));
			put32 (fileBytes, bits);
		}

		file.write (reinterpret_cast<const char*> (fileBytes.data ()),
					(std::streamsize) fileBytes.size ());

		return file.good ();
	}
} // namespace

float quantiseSample (float value, int bits)
{
	const auto clamped = std::clamp (bits, 1, kMaxBits);

	// `steps` is the number of intervals, so a one-bit shape has one and lands on two levels.
	// Scaling through 0..1 rather than -1..1 keeps the rounding symmetric at both rails: a value
	// that is already at +1 stays there instead of being rounded past it.
	//
	// ldexp rather than `1 << clamped`: at thirty-two bits that shift is undefined, and a double
	// has the fifty-three bits of mantissa this needs to land exactly on 2^32 - 1.
	const auto steps = std::ldexp (1.0, clamped) - 1.0;

	const auto scaled = (double) (std::clamp) (value, -1.0f, 1.0f) * 0.5 + 0.5;
	const auto snapped = std::round (scaled * steps) / steps;

	return (float) (snapped * 2.0 - 1.0);
}

bool writeCycleWav (const ChannelState& channel, const std::string& path,
					ExportMode mode, double seconds, int bits)
{
	if (path.empty ())
	{
		return false;
	}

	const auto cycle = cycleOf (channel);

	if (cycle.size () < 2)
	{
		return false;
	}

	// Whatever the chip produced is what gets written unless the user asked for a depth.
	const auto depth = (bits == kNativeBits) ? nativeBits (channel.waveKind)
											 : std::clamp (bits, 1, kMaxBits);

	std::vector<float> samples;
	auto sampleRate = (uint32_t) kWavetableRate;

	if (mode == ExportMode::single)
	{
		// Every source sample is held, at every depth: a thirty-two-point cycle becomes thirty-two
		// platforms however many levels sit on them. That is the shape the chip's counter produces
		// and the shape the panel draws, and a reference wavetable of the same wave contains.
		//
		// Depth is a vertical property only. An earlier version interpolated above eight bits, on
		// the argument that a thirty-two-step platform is an artefact once the levels are fine -
		// but that replaced the shape with a ramp the hardware never produced, which is a larger
		// change than the one it was trying to fix.
		samples = nearest (cycle, kWavetableSamples);
	}
	else
	{
		// One period is 1 / kReferenceHz of a second, so the rate follows from the cycle's own
		// length and the requested duration decides how many periods fit. 256 samples - the
		// longest table any backend here produces - works out to 112 kHz, inside what a sampler
		// will take; nothing longer exists to push it over.
		sampleRate = (uint32_t) std::lround (kReferenceHz * (double) cycle.size ());

		const auto duration = (std::clamp) (seconds, 0.1, 10.0);
		const auto periods = std::max (1, (int) std::lround (duration * kReferenceHz));

		samples = repeated (cycle, periods);
	}

	// Depth is applied here rather than inside the writer: the writer's job is the container, and a
	// sample file wants the same bytes whatever depth produced them.
	for (auto& sample : samples)
	{
		sample = quantiseSample (sample, depth);
	}

	// The one-period marker, for the destination that reads one period. Its format is plain ASCII
	// and was read off a reference file - "<!>" followed by the samples per cycle - and without it
	// a wavetable synth treats the file as an ordinary short recording and refuses to import it
	// onto the oscillator. A looped file is audio and gets none.
	const auto marker = (mode == ExportMode::single)
						  ? "<!>" + std::to_string (kWavetableSamples)
								+ " 00000000 wavetable (www.xferrecords.com)"
						  : std::string ();

	return writeFloat32Wav (path, samples, sampleRate, marker);
}

bool isSampledChannel (const ChannelState& channel)
{
	return channel.sampleRate > 0.0f && channel.wave.size () >= 2;
}

bool writeSampleWav (const ChannelState& channel, const std::string& path)
{
	if (path.empty () || ! isSampledChannel (channel))
	{
		return false;
	}

	// The rate the chip reads the bytes at, not a wavetable's fixed 44100: a drum sampled at
	// 11 kHz and written as 44 kHz is the same drum three octaves down and four times too long.
	const auto rate = (uint32_t) std::lround (channel.sampleRate);

	return writeFloat32Wav (path, channel.wave, rate, std::string ());
}

std::string legalFileName (const std::string& name)
{
	std::string out;
	out.reserve (name.size ());

	for (const char c : name)
	{
		switch (c)
		{
		// A space is legal everywhere and awkward everywhere: it survives a copy into a shell, a
		// URL and a sampler's file list only if the reader remembers to quote it.
		case ' ':
			out.push_back ('-');
			break;

		case '/': case '\\': case ':': case '*': case '?':
		case '"': case '<': case '>': case '|':
			out.push_back ('_');
			break;

		default:
			out.push_back (c);
			break;
		}
	}

	// Windows refuses a name ending in a space or a dot, and leading spaces read as an accident.
	// The hyphen is in both sets because a space became one a few lines up: without it, a name
	// that started as "  Wave 8  " keeps the two hyphens its leading spaces turned into.
	const auto first = out.find_first_not_of (" .-");
	const auto last = out.find_last_not_of (" .-");

	out = (first == std::string::npos) ? std::string () : out.substr (first, last - first + 1);

	return out.empty () ? std::string ("unnamed") : out;
}

ExportOutcome exportWavetables (const ChipStateSnapshot& snapshot, const std::string& folder,
								ExportMode mode, double seconds, int bits)
{
	ExportOutcome outcome;
	outcome.channelsSeen = (int) snapshot.channels.size ();

	const auto base = folder.empty () ? std::string (".") : folder;

	// A name that repeats gets a suffix rather than overwriting its neighbour. The row number this
	// used to append guaranteed uniqueness, and it cost every name a second digit that meant
	// nothing to a reader - "2A03_Pulse 1_1.wav".
	std::map<std::string, int> used;

	for (std::size_t i = 0; i < snapshot.channels.size (); ++i)
	{
		const auto& channel = snapshot.channels[(std::size_t) i];

		// A channel that carries a sample is a recording, not a shape, and it goes out as audio at
		// the rate the chip reads it - in both modes, because repeating a drum hit is not a
		// wavetable and a period of one is not a period either.
		//
		// This used to fall through to skippedSample: hasPeriodicShape() is right that a sample is
		// not a period, and the note there says "those channels want a PCM export, which does not
		// exist yet". This is that export.
		const bool isSample = isSampledChannel (channel);

		if (! isSample && ! hasPeriodicShape (channel))
		{
			++outcome.skippedSample;
			continue;
		}

		auto stem = legalFileName (channel.chipName) + "_" + legalFileName (channel.name);

		if (stem.size () > 180)
		{
			// Most filesystems stop at 255 bytes for the whole name; the suffix and extension
			// need room to land.
			stem.resize (180);
		}

		const auto seen = ++used[stem];
		const auto name = stem + (seen > 1 ? "_" + std::to_string (seen) : "") + ".wav";

		const auto path = base + "/" + name;

		const bool ok = isSample ? writeSampleWav (channel, path)
								 : writeCycleWav (channel, path, mode, seconds, bits);

		if (ok)
		{
			++outcome.written;
		}
		else
		{
			++outcome.failed;
		}
	}

	return outcome;
}

bool writeWavFile (const std::string& path, const std::vector<float>& values, double rate)
{
	// A rate that rounds to zero would make a file no decoder can place on a timeline. It cannot
	// happen from a backend that fills `rate` correctly, and it is cheaper to refuse than to write
	// a WAV whose fmt chunk says 0 Hz.
	if (rate < 0.5)
	{
		return false;
	}

	return writeFloat32Wav (path, values, (uint32_t) std::lround (rate), std::string ());
}

int writeSamples (const std::vector<DecodedSample>& samples, const std::string& folder)
{
	int written = 0;

	for (const auto& sample : samples)
	{
		if (sample.values.empty ())
		{
			continue;
		}

		const auto path = folder + "/" + legalFileName (sample.name) + ".wav";

		if (writeWavFile (path, sample.values, sample.rate))
		{
			++written;
		}
	}

	return written;
}

} // namespace toybox::playback
