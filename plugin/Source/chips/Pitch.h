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

/** Middle C's MIDI note. Every sampled voice's transposition is measured from here, which is what
	makes the pitch a note the registers alone can name (see the L1/L2 model below). */
constexpr int kMiddleCNote = 60;

/** L1 - the transposition a playback rate asks for, in cents, against the rate the chip's samples
	are taken to be recorded at.

	This layer is the one thing about a sampled voice's pitch that is *always* computable: the rate
	comes from registers and hardware constants, and the reference rate is a documented property of
	the chip (a 8 kHz DAC, a delta-N scale, a sample-rate table). Nothing here looks at the sample's
	bytes, so it cannot be wrong about what the driver asked for - only about the absolute octave,
	which is the reference's job.

	The reference is `MDPlayer`'s model, cited rather than invented: its ADPCM-B pitch is
	`deltaN * 50kHz / 2^16` divided by 8000 and quantised (`form/KB/OPL/frmY8950.cs`,
	`Common.searchSegaPCMNote`), its FM pitch is `fnum * clock / (24 * div * 2^21) * 2^(oct+2)`
	(`PianoRoll/YM2610.cs`), and both end in a nearest-frequency lookup (`common.cs`). Frequency
	first, note second - never the other way round, and never from the waveform. */
inline int transpositionCents (double rate, double referenceRate)
{
	if (rate <= 0.0 || referenceRate <= 0.0)
	{
		return 0;
	}

	return (int) std::lround (1200.0 * std::log2 (rate / referenceRate));
}

/** L1 as a MIDI note: middle C plus the transposition, with the leftover cents in `finePitch`.

	A transposition below middle C minus 60 semitones or above the top of the range is not a note
	(returns -1) - the same "no note" the note extractor already reads from `quantiseFrequency`. */
inline int quantiseTransposition (int cents, int* finePitch)
{
	if (finePitch != nullptr)
	{
		*finePitch = 0;
	}

	const int semitones = (int) std::lround ((double) cents / 100.0);
	const int note = kMiddleCNote + semitones;
	if (note < 0 || note > 127)
	{
		return -1;
	}

	if (finePitch != nullptr)
	{
		*finePitch = std::max (-99, std::min (99, cents - semitones * 100));
	}

	return note;
}

/** The three shapes a sampled decoder's `noteFor` takes - pick one, do not invent a fourth.

	1. **Transposition only** - the register is a playback rate and the chip names its reference.
	   `quantisePlaybackRate (rate, referenceRate, finePitch)`, with the sample's own cycle riding
	   beside it as the root correction (`describeSamplePitch`), never dividing it here.
	   C140, C352, OPN ADPCM-B.

	2. **Rate plus the drum verdict** - the same, but the decoder knows whether the sample is
	   melodic at all, so a drum becomes a C4 trigger instead of a wrong note.
	   `noteFromPlaybackRate (rate, referenceRate, measured, oneShotNote, finePitch)`.
	   RF5C68, SegaPCM, Ga20, QSound, K053260, K054539.

	3. **The cycle is the pitch** - there is no transposition to report; what sounds is whatever
	   the sample's own period says. `noteFromSampleCycle (rate, cycleLength, melodic, ...)`.
	   OKIM6295.

	ES5503 is deliberately outside all three: its register is a step through a table, not a rate, so
	it has no L1 and measures the whole pitch itself. That difference is real and should stay visible
	rather than be folded into shape 1 for the sake of uniformity.

	The FM, noise and one-shot tables are not sampled decoders at all and share none of this. */

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

/** The note for a sample that carries its own pitch: the cycle is the only thing that defines it.

	Two chips are in this family - OKIM6295 has one fixed DAC rate and the ES5503 walks a table - and
	neither has a transposition to report: what the chip plays is whatever the sample's own period
	says. `oneShotNote` is returned when there is nothing to measure, because "no period" is a drum,
	not a silent channel.

	The guard is the point of this function: a decoder that forgets `melodic` turns a drum into a
	wrong note, and only that chip would show it. */
inline int noteFromSampleCycle (double rate, double cycleLength, bool melodic, int oneShotNote,
								int* finePitch)
{
	if (finePitch != nullptr)
	{
		*finePitch = 0;
	}

	// a rate of zero is silence, not a drum: quantiseFrequency turns it into -1
	if (! melodic || cycleLength <= 0.0)
	{
		return oneShotNote;
	}

	return quantiseFrequency (rate / cycleLength, finePitch);
}

/** The note for a recorded sample: the transposition its rate register asks for (L1).

	Six chips describe a voice this way - the rate is the register, the reference is the chip's
	nominal sample rate, and the sample's own period rides beside it as the root correction rather
	than dividing it here. `measured` is the decoder's verdict on whether the sample is melodic at
	all; when it is false the voice is a drum and reports `oneShotNote`.

	This and `noteFromSampleCycle` are the two shapes the sampled decoders had been writing by hand. */
inline int noteFromPlaybackRate (double rate, double referenceRate, bool measured, int oneShotNote,
								 int* finePitch)
{
	if (finePitch != nullptr)
	{
		*finePitch = 0;
	}

	// a rate of zero stays silence: quantisePlaybackRate returns -1 for it
	if (! measured)
	{
		return oneShotNote;
	}

	return quantisePlaybackRate (rate, referenceRate, finePitch);
}

/** The rate an 8-bit PCM sample is taken to be recorded at when its chip names none - the reference
	the reference implementation divides a playback rate by before quantising it (`fSample / 8000`
	in MDPlayer's ADPCM pitch, `searchSegaPCMNote`), and the one the chips with no documented rate of
	their own use here. It is a convention, not a measurement: it only decides which octave the
	transposition lands in, which is precisely the part the sample's own root corrects. */
constexpr double kSampleReferenceRate = 8000.0;

/** L2 - the correction a sample's own root asks for, in cents.

	`cycleLength` is the sample's period in samples and `referenceRate` the rate the chip plays a
	sample at when its rate register holds the nominal figure. Their quotient is the pitch the sample
	sounds at that nominal rate, and C4 is that pitch by convention - so the correction is how far the
	recording actually sits from middle C. This is the *only* number the "transpose by the sample's
	root" setting adds, and the only one that cannot be computed from registers: it is 0 when the
	sample has no measurable root (a drum, an ambiguous multi-period sample, a sample with no ROM).

	Note the arithmetic: `transpositionCents (rate / cycle, middleC)` equals
	`transpositionCents (rate, refRate) + rootCentsFromCycle (cycle, refRate)`, which is what makes the
	two layers interchangeable with the old single-step mapping. */
inline int rootCentsFromCycle (double cycleLength, double referenceRate)
{
	if (cycleLength <= 0.0 || referenceRate <= 0.0)
	{
		return 0;
	}

	return transpositionCents (referenceRate / cycleLength, kMiddleCHz);
}

/** What a sampled voice's two pitch layers say, for a decoder's `channelState()`.

	Every sampled chip has the same three facts - the rate its registers ask for, the rate its ROM is
	taken to be recorded at, and (when it could be measured) the sample's own cycle - so the three
	fields are filled in one place instead of seven. `melodic` is the chip's own verdict on whether
	the sample has a pitch at all; a drum keeps its C4 trigger and reports no layers. */
struct SampledPitch
{
	double hz = 0.0;
	int transposeCents = 0;
	int rootCents = 0;
	PitchSource source = PitchSource::unpitched;
};

inline SampledPitch describeSamplePitch (double rate, double referenceRate, double cycleLength,
										 bool melodic)
{
	SampledPitch pitch;
	if (! melodic || rate <= 0.0)
	{
		return pitch;		// a drum: no layers, the caller reports the C4 trigger
	}

	pitch.hz = rate;
	pitch.transposeCents = transpositionCents (rate, referenceRate);
	pitch.rootCents = rootCentsFromCycle (cycleLength, referenceRate);
	pitch.source = (pitch.rootCents != 0) ? PitchSource::sampleInferred
										 : PitchSource::registerExact;
	return pitch;
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
	for (int lag = 1; lag <= maxLag; ++lag)
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

	// A period is a lag the signal *returns* to: the correlation has to fall away in between. A
	// drum's attack is smooth, so its correlation stays high at every short lag and the shortest
	// lag wins by being smooth rather than by being a period - the K054539 drum kit in
	// test_file/K054539 came out as "eight samples", which is 2.6 kHz of nothing. A waveform with
	// a real period decorrelates and then re-correlates, so the trough is the discriminating test.
	double trough = 1.0;
	for (int lag = 1; lag < chosen; ++lag)
	{
		trough = std::min (trough, correlation[(std::size_t) lag]);
	}

	if (trough > 0.5)
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
