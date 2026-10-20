#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace toybox::chips
{

/** Turns a frequency into the nearest MIDI note plus the distance from it in cents.

	Every decoder's note table ends up with a frequency - the chip's dividers differ, the reference
	pitch does not - so this is the one place that knows where A4 is. Returns -1 for a frequency
	outside the MIDI range, which the note extractor reads as "no note". */
inline int quantiseFrequency (double hz, int* finePitch)
{
	if (finePitch != nullptr)
	{
		*finePitch = 0;
	}

	if (hz <= 0.0)
	{
		return -1;
	}

	const double exact = 69.0 + 12.0 * std::log2 (hz / 440.0);
	const int rounded = (int) std::lround (exact);
	if (rounded < 0 || rounded > 127)
	{
		return -1;
	}

	if (finePitch != nullptr)
	{
		*finePitch = (std::int16_t) std::lround ((exact - (double) rounded) * 100.0);
	}

	return rounded;
}

/** Middle C: the pitch a sample player's ROM is taken to stand at when it plays back at its
	recorded rate. */
constexpr double kMiddleCHz = 261.6255653005986;

/** Turns the playback rate of a recorded sample into a note.

	A sampled voice carries no pitch in its registers, only the number of bytes a second the chip
	advances the sample pointer at. The pitch it sounds is that rate divided by the rate the ROM
	was recorded at - which is in no register either. So the mapping needs a reference: `rootRate`
	is the rate at which the ROM plays back at its own pitch, and that playback is taken to be
	middle C. Chips differ in how they derive the rate, and their drivers in what they call the
	root, so the root is an argument rather than a constant; the MIDI output settings will make it a
	preference (design doc section 11.1), which is why it is not baked into any one decoder.

	Returns -1 for a zero rate, which the note extractor reads as "silent". */
inline int quantisePlaybackRate (double rate, double rootRate, int* finePitch)
{
	if (finePitch != nullptr)
	{
		*finePitch = 0;
	}

	if (rate <= 0.0 || rootRate <= 0.0)
	{
		return -1;
	}

	return quantiseFrequency (kMiddleCHz * rate / rootRate, finePitch);
}

/** The length of one cycle of a recorded waveform, in samples (bytes for an 8-bit player).

	None of the sampled players states this number: their registers hold a playback rate, and the
	pitch a sample sounds is that rate divided by the length of one cycle of the waveform stored in
	the ROM. So the cycle has to be measured from the bytes themselves, and the part a sustained
	note repeats - the loop region - is the part worth measuring: it does not depend on where the
	sample began or on an attack transient. The loop also refines the result, because a driver
	loops a whole number of cycles: the estimate is snapped to `loopLength / round (loopLength /
	period)`, which turns a period good to a fraction of a sample into one good to a fraction of a
	cent over the whole loop.

	`waveform` is the sample region from its first sample; `loopOffset` and `loopLength` describe
	the part that repeats (pass 0 when the sample does not loop). Values may be linear PCM or any
	monotonic companding of it (mu-law): the period is a repetition in time, which companding does
	not move.

	Returns 0 for a waveform whose period does not stand out - a drum, noise, a one-shot with no
	loop, a sample too short to hold a cycle - which the callers read as "use the root rate". */
inline double estimateCycleLength (const std::vector<double>& waveform, std::size_t loopOffset,
								   std::size_t loopLength)
{
	// a window of two kilobytes is several cycles of the lowest note these players hold, and one
	// kilobyte of lag reaches a 8 Hz cycle
	constexpr std::size_t kWindowSamples = 2048;
	constexpr int kMinLag = 8;
	constexpr int kMaxLag = 1024;

	const std::size_t size = waveform.size();
	if (size < 64)
	{
		return 0.0;
	}

	std::size_t begin = 0;
	std::size_t length = size;
	if (loopLength >= 128 && loopOffset + loopLength <= size)
	{
		begin = loopOffset;
		length = loopLength;
	}

	length = std::min (length, kWindowSamples);
	if (length < 4 * (std::size_t) kMinLag)
	{
		return 0.0;
	}

	const double* x = waveform.data() + begin;
	double mean = 0.0;
	for (std::size_t i = 0; i < length; ++i)
	{
		mean += x[i];
	}

	mean /= (double) length;

	const int maxLag = (int) std::min<std::size_t> (length / 2, (std::size_t) kMaxLag);
	if (maxLag <= kMinLag)
	{
		return 0.0;
	}

	std::vector<double> correlation ((std::size_t) maxLag + 2, 0.0);
	for (int lag = kMinLag; lag <= maxLag; ++lag)
	{
		double product = 0.0;
		double energyA = 0.0;
		double energyB = 0.0;
		for (std::size_t i = 0; i + (std::size_t) lag < length; ++i)
		{
			const double a = x[i] - mean;
			const double b = x[i + (std::size_t) lag] - mean;
			product += a * b;
			energyA += a * a;
			energyB += b * b;
		}

		correlation[(std::size_t) lag] = (energyA > 0.0 && energyB > 0.0)
											 ? product / std::sqrt (energyA * energyB)
											 : 0.0;
	}

	// the strongest local maximum: a period is a lag the signal repeats at, and a maximum rather
	// than a shoulder of one keeps a harmonic from winning by being next to it
	double best = 0.0;
	for (int lag = kMinLag; lag <= maxLag; ++lag)
	{
		const double value = correlation[(std::size_t) lag];
		if (value > correlation[(std::size_t) (lag - 1)] && value >= correlation[(std::size_t) (lag + 1)])
		{
			best = std::max (best, value);
		}
	}

	if (best < 0.80)
	{
		return 0.0;		// nothing repeats: a drum, noise, or a sample too short to hold a cycle
	}

	// the shortest lag that is as good as the best one: a signal that repeats every L samples also
	// repeats every 2 L, and the note is the fundamental, not the octave
	int chosen = -1;
	const double acceptable = best - 0.02;
	for (int lag = kMinLag; lag <= maxLag; ++lag)
	{
		const double value = correlation[(std::size_t) lag];
		if (value >= acceptable && value > correlation[(std::size_t) (lag - 1)]
			&& value >= correlation[(std::size_t) (lag + 1)])
		{
			chosen = lag;
			break;
		}
	}

	if (chosen < 0)
	{
		return 0.0;
	}

	// parabolic interpolation: the true period rarely lands on a whole lag
	double period = (double) chosen;
	if (chosen > kMinLag && chosen < maxLag)
	{
		const double before = correlation[(std::size_t) (chosen - 1)];
		const double here = correlation[(std::size_t) chosen];
		const double after = correlation[(std::size_t) (chosen + 1)];
		const double curvature = before - 2.0 * here + after;
		if (curvature < 0.0)
		{
			period += std::max (-0.5, std::min (0.5, 0.5 * (before - after) / curvature));
		}
	}

	if (period <= 0.0)
	{
		return 0.0;
	}

	// the loop is a whole number of cycles, and that constraint is far more precise than the
	// correlation: a 6770-byte loop of 89 cycles states the period to a hundredth of a byte
	if (loopLength >= 128)
	{
		const double cycles = (double) loopLength / period;
		const double rounded = std::floor (cycles + 0.5);
		if (rounded >= 1.0 && std::fabs (cycles - rounded) < 0.35)
		{
			period = (double) loopLength / rounded;
		}
	}

	return period;
}

} // namespace toybox::chips
