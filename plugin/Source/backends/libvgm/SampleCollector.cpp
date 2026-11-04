#include "SampleCollector.h"

#include "chips/NesDpcmSample.h"
#include "chips/Okim6295Codec.h"

namespace toybox::backends::libvgm
{

void SampleCollector::add (uint64_t key, const std::string& name, const uint8_t* bytes,
						   std::size_t length, SampleFormat format, double rate, uint32_t sampleId,
						   int skipNibbles)
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
	else if (format == SampleFormat::qsoundAdpcm)
	{
		// The QSound's adaptive delta, from `adpcm_update` in qsound_ctr.c. Not a table of fixed
		// steps like the two four-bit formats above: each nibble's size is scaled by a running
		// `step_size` that grows and shrinks with the signal, which is what makes this one a
		// compressor rather than a lookup.
		//
		// A byte carries two nibbles and the *high* one comes first - the opposite of both Konami
		// formats - and only the low nibble advances the address.
		static constexpr int kStepTable[16] =
			{ 154, 154, 128, 102, 77, 58, 58, 58, 58, 58, 58, 58, 77, 102, 128, 154 };

		sample.values.reserve (length * 2);

		int stepSize = 10;
		int accumulator = 0;

		for (std::size_t i = 0; i < length; ++i)
		{
			const uint8_t byte = bytes[i];

			// High nibble first, then low - and both are signed four-bit values.
			const int nibbles[2] = { (int) ((int8_t) (byte & 0xF0) >> 4),
									 (int) ((int8_t) (byte << 4) >> 4) };

			for (const int nibble : nibbles)
			{
				// delta = (0.5 + abs(step)) * step_size, negated when the step is not positive.
				int delta = ((1 + std::abs (nibble * 2)) * stepSize) >> 1;

				if (nibble <= 0)
				{
					delta = -delta;
				}

				accumulator = (std::min) ((std::max) (accumulator + delta, -32768), 32767);
				sample.values.push_back ((float) accumulator);

				stepSize = (kStepTable[8 + nibble] * stepSize) >> 6;
				stepSize = (std::min) ((std::max) (stepSize, 1), 2000);
			}
		}

		// Normalised to its own peak, like the two four-bit formats and for the same reason.
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
	else if (format == SampleFormat::signMagnitude8)
	{
		// Not two's complement, which is what every other eight-bit chip here uses.
		//
		// `rf5c68.c:159` does `if (sample & 0x80) out += sample & 0x7f; else out -= sample;` -
		// bit 7 is a sign and the low seven bits are the distance from zero. So 0x80..0xFF runs
		// 0..+127 and 0x00..0x7F runs 0..-127, with silence at both ends and nothing in between.
		//
		// Reading it as two's complement mirrors the two halves and shifts the positive one down
		// by 128, which is not a phase flip - it collapses every sample into a buzz while the bytes
		// themselves are perfectly good. That is what the first export of these tracks sounded like.
		sample.values.reserve (length);

		for (std::size_t i = 0; i < length; ++i)
		{
			const uint8_t byte = bytes[i];
			const float magnitude = (float) (byte & 0x7F) / 128.0f;
			sample.values.push_back ((byte & 0x80) ? magnitude : -magnitude);
		}
	}
	else if (format == SampleFormat::msm6295)
	{
		// The OKI's delta codec, through the same header the decoder measures the sample's cycle
		// with. Two independent implementations would let the exported file and the drawn row
		// disagree, and both would still look like waveforms - the disagreement would never fail
		// anything.
		//
		// High nibble first, which is the chip's order and the opposite of the two Konami formats
		// above. `length` counts bytes; a byte carries two levels.
		// `skipNibbles` exists for this format alone: the chip's start address counts nibbles, so a
		// sample can begin on the low half of a byte. Ignoring that would shift every level by one
		// - a click at the start and a waveform one step out of phase with what the row draws.
		const uint32_t available = (uint32_t) (length * 2);
		const uint32_t skip = (uint32_t) (std::max (0, std::min (skipNibbles, (int) available)));

		const std::vector<double> decoded = chips::okim::decode (
			[&bytes, length] (uint32_t address)
			{
				return (address < length) ? bytes[address] : (uint8_t) 0xFF;
			},
			skip, available - skip);

		float peak = 1.0f;

		sample.values.reserve (decoded.size ());

		for (const double level : decoded)
		{
			sample.values.push_back ((float) (level / chips::okim::kFullScale));
			peak = (std::max) (peak, std::abs (sample.values.back ()));
		}

		// Normalised to its own peak, like the other delta formats: the codec's accumulator has no
		// absolute scale to trust, and a sample recorded quietly would otherwise export as silence.
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
