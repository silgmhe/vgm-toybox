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
	else if (format == SampleFormat::dpcm4 || format == SampleFormat::kadpcm4)
	{
		// Two four-bit delta formats, one character apart: index 8 of the table is -128 on the
		// K053260 and 0 on the K054539.
		//
		// The K054539 needs the 0 because its end marker is the byte 0x88 - two nibbles of 8 - and a
		// -128 there would make a terminator read as two full-scale downward steps. The K053260 has
		// no marker (it counts bytes against a length register), so its table is the hardware's own
		// (`kadpcm_table` in k053260.c). Using either table for the other chip produces a waveform
		// that is wrong in a way nobody would call obviously broken: every 8 drifts the sum.
		static constexpr int kDpcmTable[16] =
			{ 0, 1, 2, 4, 8, 16, 32, 64, 0, -64, -32, -16, -8, -4, -2, -1 };
		static constexpr int kKadpcmTable[16] =
			{ 0, 1, 2, 4, 8, 16, 32, 64, -128, -64, -32, -16, -8, -4, -2, -1 };

		const int* deltas = (format == SampleFormat::kadpcm4) ? kKadpcmTable : kDpcmTable;

		const std::size_t count = length * 2;
		int accumulator = 0;

		sample.values.reserve (count);

		for (std::size_t i = 0; i < count; ++i)
		{
			const uint8_t byte = bytes[i / 2];
			const int nibble = (i % 2 == 0) ? (byte & 0x0F) : (byte >> 4);
			accumulator += deltas[nibble];

			// A guard, not a mode: an ordinary sample wanders by hundreds, and reaching 32768 means
			// the data is not what the register said it was. Clamping keeps such a sample visible
			// instead of letting it run off the number line.
			accumulator = (std::min) ((std::max) (accumulator, -32768), 32767);
			sample.values.push_back ((float) accumulator);
		}

		// The sum has no fixed full scale - its largest step is 128 and it wanders - so it is
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
