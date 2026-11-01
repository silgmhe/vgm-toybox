#include "SampleCollector.h"

#include "chips/NesDpcmSample.h"

namespace toybox::backends::libvgm
{

void SampleCollector::add (uint64_t key, const std::string& name, const uint8_t* bytes,
						   std::size_t length, SampleFormat format, double rate, uint32_t sampleId)
{
	if (bytes == nullptr || length < 2)
	{
		return;
	}

	if (! seen.emplace (key, true).second)
	{
		return;
	}

	playback::DecodedSample sample;
	sample.name = name;
	sample.rate = rate;
	sample.sampleId = sampleId;

	if (format == SampleFormat::nesDelta)
	{
		// The 2A03 expands each byte into the eight one-bit steps the DAC takes.
		sample.values = chips::dpcm::decode (bytes, length);
	}
	else
	{
		sample.values.reserve (length);

		for (std::size_t i = 0; i < length; ++i)
		{
			sample.values.push_back ((float) (int8_t) bytes[i] / 128.0f);
		}
	}

	out.push_back (std::move (sample));
}

std::vector<playback::DecodedSample> SampleCollector::take (void)
{
	seen.clear ();

	return std::move (out);
}

} // namespace toybox::backends::libvgm
