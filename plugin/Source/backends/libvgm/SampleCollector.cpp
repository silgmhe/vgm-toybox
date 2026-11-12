#include "SampleCollector.h"

#include "chips/NesDpcmSample.h"
#include "chips/Okim6295Codec.h"
#include "chips/OpnAdpcmB.h"

namespace toybox::backends::libvgm
{

void SampleCollector::add (uint64_t key, const std::string& name, const uint8_t* bytes,
						   std::size_t length, SampleFormat format, double rate, uint32_t sampleId,
					   bool isCycle)
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
	sample.isCycle = isCycle;

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
		const std::vector<double> decoded = chips::okim::decode (
			[&bytes, length] (uint32_t address)
			{
				return (address < length) ? bytes[address] : (uint8_t) 0xFF;
			},
			0, (uint32_t) (length * 2));

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
	else if (format == SampleFormat::vsu6)
	{
		// **Six bits, silence at 0x20.** `vsu.c:229` stores `V & 0x3F`, and `vsu.c:402` subtracts
		// 0x20 before mixing, so an entry is a signed level in -32..+31. No normalisation: the scale
		// is absolute, and rescaling would make a quiet table as loud as a loud one.
		sample.values.reserve (length);

		for (std::size_t i = 0; i < length; ++i)
		{
			sample.values.push_back ((float) (((int) bytes[i] & 0x3F) - 0x20) / 32.0f);
		}
	}
	else if (format == SampleFormat::prom4)
	{
		// **Four bits, silence at 8.** The K005289's PROM holds one level per byte (the block is
		// already expanded) and the core subtracts 8 before mixing (`k005289.c:198`), so the value
		// is a signed level in -8..+7 and the scale is eight.
		sample.values.reserve (length);

		for (std::size_t i = 0; i < length; ++i)
		{
			sample.values.push_back ((float) (((int) bytes[i] & 0x0F) - 8) / 8.0f);
		}
	}
	else if (format == SampleFormat::pcm8Unsigned)
	{
		// **Eight bits, silence at 0x80, and no terminator.** GA20 and the ES5503 stop at a 0x00
		// byte, which works because their samples end that way; the Sega PCM's do not - the length
		// comes from the end-address register - so the caller's `length` is the only bound and a
		// plain centred byte is the whole decode.
		sample.values.reserve (length);

		for (std::size_t i = 0; i < length; ++i)
		{
			sample.values.push_back ((float) (((int) bytes[i] - 0x80)) / 128.0f);
		}
	}
	else if (format == SampleFormat::adpcmA4)
	{
		// **ADPCM-A, exactly as `fmopn.c` runs it.** A nibble picks a delta out of `steps[]` scaled
		// by the running step; its low three bits then move the step by `step_inc`. The accumulator
		// is twelve bits and **wraps** (MSM5205 heritage), unlike Delta-T which clamps. One nibble
		// is one sample, high nibble of a byte first.
		static constexpr int stepTable[49] = {
			16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73,
			80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
			337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
			1282, 1411, 1552
		};
		static constexpr int stepInc[8] = { -16, -16, -16, -16, 32, 80, 112, 144 };

		int accumulator = 0;
		int step = 0;

		sample.values.reserve (length * 2);

		for (std::size_t i = 0; i < length; ++i)
		{
			for (int half = 0; half < 2; ++half)
			{
				const int nibble = (half == 0) ? ((bytes[i] >> 4) & 0x0F) : (bytes[i] & 0x0F);
				const int delta = (2 * (nibble & 7) + 1) * stepTable[step / 16] / 8;

				accumulator = (accumulator + ((nibble & 8) != 0 ? -delta : delta)) & 0xFFF;
				step = std::max (0, std::min (48 * 16, step + stepInc[nibble & 7]));

				const int value = (accumulator & 0x800) != 0 ? (accumulator | ~0xFFF) : accumulator;
				sample.values.push_back ((float) value / 2048.0f);
			}
		}
	}
	else if (format == SampleFormat::adpcmB4)
	{
		// **Delta-T, decoded by the same function the panel and the root estimate use.** The
		// format is one nibble per level, high nibble first, and the accumulator clamps at 16 bits
		// instead of wrapping (that is ADPCM-A). A decoder that wrapped here produces a plausible
		// waveform that is wrong on every loud sample, which is why there is one implementation.
		toybox::chips::opn::deltaT::decode (bytes, length, sample.values);
	}
	else if (format == SampleFormat::ga20 || format == SampleFormat::es5503)
	{
		// **Eight-bit unsigned with 0x80 as silence, and 0x00 as the end.** `iremga20.c:177` reads
		// `rom[pos] - 0x80` into an `int8_t`; `iremga20.c:174` stops playback on a zero byte, so
		// that byte is a terminator and not a level - the same shape as the K007232's bit-7 marker,
		// with a different centre and a different marker value.
		//
		// No peak normalisation: the format has an absolute scale, and rescaling would make a quiet
		// sample as loud as a loud one.
		sample.values.reserve (length);

		for (std::size_t i = 0; i < length; ++i)
		{
			if (bytes[i] == 0x00)
			{
				break;
			}

			sample.values.push_back ((float) ((int) bytes[i] - 0x80) / 128.0f);
		}
	}
	else if (format == SampleFormat::k007232)
	{
		// **Seven-bit unsigned, `0x40` is silence, and the end is in the data.** `k007232.c:232`
		// reads `(value & 0x7F) - 0x40`; `k007232.c:246` restarts the voice when a byte has bit 7
		// set, so that byte is the terminator and never part of the sample. `length` is what the
		// caller could show of the window; this stops earlier, at the marker.
		sample.values.reserve (length);

		for (std::size_t i = 0; i < length; ++i)
		{
			const uint8_t value = bytes[i];

			if ((value & 0x80u) != 0)
			{
				break;
			}

			// Already -1..1: 0x00 is -1, 0x40 is 0, 0x7F is just under 1. No peak normalisation -
			// unlike the delta formats, this one has an absolute scale and normalising would make
			// a quiet sample as loud as a loud one.
			sample.values.push_back ((float) ((int) (value & 0x7Fu) - 0x40) / 64.0f);
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
