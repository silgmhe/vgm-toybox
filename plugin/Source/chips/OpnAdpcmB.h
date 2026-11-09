#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace toybox::chips::opn
{

/** **Delta-T's nibble decoder, in one place.**

	Three readers want it: the panel's shape (through `OpnDecoder::channelState`), the cycle estimate
	that turns a sample into a root (`adpcmBCycleFor`), and the exporter, which has to produce the same
	levels the panel draws. They used to be two loops inside `OpnDecoder.cpp`; the exporter would have
	been a third copy, and a third copy of a step table is how two paths start disagreeing.

	The tables and the accumulation are the core's `ymdeltat.c`: a 4-bit delta picks an entry from
	`kStepB1` scaled by the running step, and the low three bits of the delta scale the step for the
	next nibble with `kStepB2`. **The accumulator clamps at 16 bits**, which is what separates Delta-T
	from the six ADPCM-A voices (theirs wraps at 12); a decoder that wraps here produces a waveform
	that looks plausible and is wrong on every loud sample.
*/
namespace deltaT
{

constexpr int kStepB1[16] = { 1, 3, 5, 7, 9, 11, 13, 15,
							  -1, -3, -5, -7, -9, -11, -13, -15 };
constexpr int kStepB2[16] = { 57, 57, 57, 57, 77, 102, 128, 153,
							  57, 57, 57, 57, 77, 102, 128, 153 };
constexpr int kStepMin = 127;
constexpr int kStepMax = 24576;

/** One nibble: adds its delta, clamps, then scales the step for the next one.

	Integer division on purpose - `kStepB1[nibble] * step / 8` and `step * kStepB2[nibble] / 64` are
	what the core computes, and rounding them differently would move every level by a fraction of a
	step. The return value is the new level as an integer, which is what the accumulator always is.
*/
inline int nextLevel (int nibble, double& accumulator, int& step)
{
	accumulator += (double) (kStepB1[nibble] * step / 8);
	accumulator = std::clamp (accumulator, -32768.0, 32767.0);
	step = std::clamp (step * kStepB2[nibble] / 64, kStepMin, kStepMax);
	return (int) accumulator;
}

/** Decodes `length` bytes, high nibble of each byte first, into -1..1 levels.

	Appends rather than assigns: the exporter builds one sample per call, but a caller that decodes a
	longer region in pieces does not have to concatenate afterwards.
*/
inline void decode (const uint8_t* bytes, std::size_t length, std::vector<float>& out)
{
	double accumulator = 0.0;
	int step = kStepMin;

	out.reserve (out.size () + length * 2);

	for (std::size_t i = 0; i < length; ++i)
	{
		for (int half = 0; half < 2; ++half)
		{
			const int nibble = (half == 0) ? ((bytes[i] >> 4) & 0x0F) : (bytes[i] & 0x0F);
			out.push_back ((float) (nextLevel (nibble, accumulator, step) / 32768.0));
		}
	}
}

} // namespace deltaT

} // namespace toybox::chips::opn
