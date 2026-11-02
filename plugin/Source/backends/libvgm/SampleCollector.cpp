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
	else if (format == SampleFormat::pcm16Signed)
	{
		// Little endian, and the length is in bytes: half as many levels as bytes. A region with
		// fewer than two levels is not a waveform, and `length < 2` above does not cover that.
		const std::size_t count = length / 2;

		if (count < 2)
		{
			return;
		}

		sample.values.reserve (count);

		for (std::size_t i = 0; i < count; ++i)
		{
			const int16_t level = (int16_t) ((uint16_t) bytes[i * 2]
										  | ((uint16_t) bytes[i * 2 + 1] << 8));
			sample.values.push_back ((float) level / 32768.0f);
		}
	}
	else if (format == SampleFormat::dpcm4)
	{
		// Four-bit deltas, low nibble first, summed into levels. The table is `k054539.c`'s
		// (`kDpcmTable`), the same one the decoder integrates to measure a pitch - a second copy
		// because that one is private to the chip and this one has to run without it.
		static constexpr int kDeltas[16] =
			{ 0, 1, 2, 4, 8, 16, 32, 64, 0, -64, -32, -16, -8, -4, -2, -1 };

		const std::size_t count = length * 2;
		int accumulator = 0;

		sample.values.reserve (count);

		for (std::size_t i = 0; i < count; ++i)
		{
			const uint8_t byte = bytes[i / 2];
			const int nibble = (i % 2 == 0) ? (byte & 0x0F) : (byte >> 4);
			accumulator += kDeltas[nibble];
			sample.values.push_back ((float) accumulator);
		}

		// The sum has no fixed full scale - its largest step is 64 and it wanders - so it is
		// normalised to its own peak, exactly as the display does. Dividing by 32768 instead left
		// every four-bit sample flat against the centre line.
		float peak = 1.0f;

		for (const float level : sample.values)
		{
			peak = (std::max) (peak, std::abs (level));
		}

		for (float& level : sample.values)
		{
			level /= peak;
		}
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
