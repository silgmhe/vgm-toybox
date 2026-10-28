#include "WaveExport.h"

#include "WaveShape.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
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

	/** `cycle` stretched to `samples`, linearly, wrapping at the end.

		Linear rather than nearest-neighbour: a wavetable is read back as a line, and a 32-step
		staircase stretched to 2048 would keep all of its stair edges as high-frequency content
		that the shape never had. Linear interpolation smooths those edges away and leaves the
		corners of a square wave where they were.

		The wrap matters. Without it the interpolation between the last sample and the first - the
		step a periodic shape cannot avoid - would be skipped, and a square wave whose duty puts
		the edge there would come out with a ramp joining its two extremes.
	*/
	std::vector<float> resample (const std::vector<float>& cycle, std::size_t samples)
	{
		std::vector<float> out;
		out.reserve (samples);

		const auto size = cycle.size ();
		const auto last = (std::size_t) (size - 1);

		for (std::size_t i = 0; i < samples; ++i)
		{
			// Position in source samples, as a fraction so the wrap is a modulo on indices.
			const double position = (double) i * (double) size / (double) samples;
			const auto index = (std::size_t) position;
			const auto fraction = (float) (position - (double) index);

			const auto next = (index >= last) ? (std::size_t) 0 : index + 1;

			const float a = cycle[std::min (index, last)];
			const float b = cycle[std::min (next, last)];

			out.push_back (a + (b - a) * fraction);
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

bool writeCycleWav (const ChannelState& channel, const std::string& path)
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

	const auto shape = resample (cycle, kWavetableSamples);

	const auto sampleRate = (uint32_t) kWavetableRate;
	const auto dataBytes = (uint32_t) shape.size () * 2u;

	std::vector<uint8_t> header;
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

	for (const float sample : shape)
	{
		const float value = (std::clamp) (sample, -1.0f, 1.0f);
		const auto pcm = (int16_t) std::lround (value * 32000.0f);

		const char bytes[2] = { (char) (pcm & 0xFF), (char) ((pcm >> 8) & 0xFF) };
		file.write (bytes, 2);
	}

	return file.good ();
}

} // namespace toybox::playback
