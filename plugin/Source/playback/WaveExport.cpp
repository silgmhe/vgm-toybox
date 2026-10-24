#include "WaveExport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <vector>

namespace toybox::playback
{
namespace
{
	void put32 (std::vector<std::uint8_t>& out, std::uint32_t value)
	{
		out.push_back ((std::uint8_t) (value & 0xFF));
		out.push_back ((std::uint8_t) ((value >> 8) & 0xFF));
		out.push_back ((std::uint8_t) ((value >> 16) & 0xFF));
		out.push_back ((std::uint8_t) ((value >> 24) & 0xFF));
	}

	void put16 (std::vector<std::uint8_t>& out, std::uint16_t value)
	{
		out.push_back ((std::uint8_t) (value & 0xFF));
		out.push_back ((std::uint8_t) ((value >> 8) & 0xFF));
	}
}

bool writeCycleWav (const ChannelState& channel, const std::string& path, int repeats)
{
	if (channel.wave.size () < 2 || path.empty ())
	{
		return false;
	}

	const int cycleLength = (int) channel.wave.size ();
	const int copies = std::max (1, repeats);
	const int frames = cycleLength * copies;

	// The rate that makes the cycle sound at the note it was playing. Outside 4 kHz..192 kHz a
	// sampler will refuse it or resample it into something unrecognisable, so the file falls back
	// to a plain 44100 and becomes a shape rather than a note.
	double rate = 44100.0;

	if (channel.frequency > 0.0f)
	{
		const double natural = (double) channel.frequency * (double) cycleLength;

		if (natural >= 4000.0 && natural <= 192000.0)
		{
			rate = natural;
		}
	}

	const auto sampleRate = (std::uint32_t) std::lround (rate);
	const std::uint32_t dataBytes = (std::uint32_t) frames * 2u;

	std::vector<std::uint8_t> header;
	header.reserve (44);

	header.insert (header.end (), { 'R', 'I', 'F', 'F' });
	put32 (header, 36u + dataBytes);
	header.insert (header.end (), { 'W', 'A', 'V', 'E' });
	header.insert (header.end (), { 'f', 'm', 't', ' ' });
	put32 (header, 16u);					// PCM chunk size
	put16 (header, 1u);						// PCM
	put16 (header, 1u);						// mono
	put32 (header, sampleRate);
	put32 (header, sampleRate * 2u);		// byte rate
	put16 (header, 2u);						// block align
	put16 (header, 16u);					// bits per sample
	header.insert (header.end (), { 'd', 'a', 't', 'a' });
	put32 (header, dataBytes);

	std::ofstream file (path, std::ios::binary | std::ios::trunc);

	if (! file)
	{
		return false;
	}

	file.write (reinterpret_cast<const char*> (header.data ()), (std::streamsize) header.size ());

	for (int copy = 0; copy < copies; ++copy)
	{
		for (int i = 0; i < cycleLength; ++i)
		{
			const float value = (std::clamp) (channel.wave[(std::size_t) i], -1.0f, 1.0f);
			const auto pcm = (std::int16_t) std::lround (value * 32000.0f);

			const char bytes[2] = { (char) (pcm & 0xFF), (char) ((pcm >> 8) & 0xFF) };
			file.write (bytes, 2);
		}
	}

	return file.good ();
}

} // namespace toybox::playback
