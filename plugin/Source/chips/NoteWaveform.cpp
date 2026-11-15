#include "NoteWaveform.h"

#include <algorithm>
#include <cmath>

namespace toybox::chips::noteWave
{
namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

/** YM2612's ML table: index 0 is half speed, the rest climb to fifteen times. */
constexpr double kMultiple[16] = {
	0.5, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 10.0, 12.0, 12.0, 15.0, 15.0
};

/** One operator's patch fields, read out of the raw registers. */
struct Operator
{
	double multiple = 1.0;
	double level = 1.0;			// 0..1; TL is an attenuation, 0 is loudest
	double attack = 0.0;		// seconds for a full sweep, from AR
	double decay = 0.0;			// from DR
	double sustainRate = 0.0;	// from SR
	double release = 0.0;		// from RR
	double sustainLevel = 1.0;	// 0..1, from SL

	double phase = 0.0;
	double envelope = 0.0;
};

/** **How long one full 0..1 sweep takes at this rate code.**

	Not the chip's step table: that one is a 16.16 counter against a divided clock, with key
	scaling on top, and reproducing it here would be a second emulator. This maps the code onto an
	exponential curve with the right *range* - 31 is a few milliseconds, 1 is several seconds - so
	an envelope of AR 31 looks instant and one of AR 8 looks slow, which is what the eye is being
	asked about.
*/
double secondsPerSweep (int rate)
{
	const int code = std::clamp (rate, 1, 31);
	return 0.004 * std::pow (2.0, (31.0 - (double) code) / 4.0);
}

uint8_t regAt (const chips::VoicePatch& patch, uint8_t offset)
{
	for (const auto& reg : patch.regs)
	{
		if (reg.first == offset)
		{
			return reg.second;
		}
	}

	return 0;
}

/** **Which operator modulates which, and who reaches the output.**

	Read off `fmopn.c`'s `setup_connection()`, which is the chip's own table. What is left out is
	the per-sample plumbing - the `MEM` delay slot that lets a carrier feed back into the next
	operator - because at the resolution this is drawn at it changes the texture of a waveform and
	not its shape.

	Index is the algorithm; `modulate[i]` is who modulates operator `i` (-1 for none), and
	`carrier[i]` says whether that operator reaches the mix.
*/
struct Algorithm
{
	int8_t modulate[4];
	bool carrier[4];
};

constexpr Algorithm kAlgorithms[8] = {
	{ { -1, 0, 1, 2 }, { false, false, false, true } },		// 0: 1 -> 2 -> 3 -> 4
	{ { -1, -1, 0, 2 }, { false, false, false, true } },		// 1: (1 + 2) -> 3 -> 4
	{ { -1, -1, -1, 0 }, { false, false, true, true } },		// 2: 1 -> 4, 3 -> 4
	{ { -1, 0, -1, 1 }, { false, false, false, true } },		// 3: 1 -> 2 -> 4, 3 -> 4
	{ { -1, 0, -1, 2 }, { false, true, false, true } },		// 4: 1 -> 2, 3 -> 4
	{ { -1, 0, 0, 0 }, { false, true, true, true } },		// 5: 1 -> (2, 3, 4)
	{ { -1, 0, 0, -1 }, { false, true, true, true } },		// 6: 1 -> 2, 1 -> 3
	{ { -1, -1, -1, -1 }, { true, true, true, true } }		// 7: all four in parallel
};

/** The four operators of `patch`, read from its registers.

	`channel` is the low two bits of the addresses the patch came back with, kept so a patch for
	channel 3 is not read as channel 0's.
*/
bool readOperators (const chips::VoicePatch& patch, Operator (&ops)[4])
{
	uint8_t channel = 0xFF;

	for (const auto& reg : patch.regs)
	{
		if (reg.first >= 0x30 && reg.first <= 0xB0)
		{
			const uint8_t bits = (uint8_t) (reg.first & 0x03);
			if (channel != 0xFF && channel != bits)
			{
				return false;		// more than one channel's registers in here
			}

			channel = bits;
		}
	}

	if (channel == 0xFF)
	{
		return false;
	}

	for (int op = 0; op < 4; ++op)
	{
		const uint8_t base = (uint8_t) (op << 2 | channel);

		const uint8_t dtMulti = regAt (patch, (uint8_t) (0x30 | base));
		const uint8_t level = regAt (patch, (uint8_t) (0x40 | base));
		const uint8_t attack = regAt (patch, (uint8_t) (0x50 | base));
		const uint8_t decay = regAt (patch, (uint8_t) (0x60 | base));
		const uint8_t sustain = regAt (patch, (uint8_t) (0x70 | base));
		const uint8_t release = regAt (patch, (uint8_t) (0x80 | base));

		Operator& o = ops[op];
		o.multiple = kMultiple[dtMulti & 0x0F];
		o.level = std::pow (10.0, -(double) (level & 0x7F) * 0.75 / 20.0);
		o.attack = secondsPerSweep ((attack >> 4) & 0x1F);
		o.decay = secondsPerSweep (decay & 0x1F);
		o.sustainRate = secondsPerSweep (sustain & 0x1F);
		o.release = secondsPerSweep (release & 0x0F);
		o.sustainLevel = 1.0 - (double) ((release >> 4) & 0x0F) / 15.0;
		o.envelope = 0.0;
	}

	return true;
}

/** Advances one operator's envelope by `dt` seconds, in the chip's four stages.

	The stages are keyed off `elapsed` rather than a state machine: a note starts at key-on, and
	the only transition that needs deciding is the move to release, which the caller signals.
*/
void advanceEnvelope (Operator& op, bool releasing, double sinceOn, double dt)
{
	if (releasing)
	{
		op.envelope = std::max (0.0, op.envelope - dt / std::max (op.release, 1e-6));
		return;
	}

	if (sinceOn < op.attack)
	{
		op.envelope = sinceOn / std::max (op.attack, 1e-6);
		return;
	}

	const double sinceDecay = sinceOn - op.attack;

	if (sinceDecay < op.decay)
	{
		op.envelope = 1.0 - (1.0 - op.sustainLevel) * (sinceDecay / std::max (op.decay, 1e-6));
		return;
	}

	// Sustain: the level falls towards zero at the sustain rate, which is what D2R means on this
	// chip - a patch with a slow SR holds, one with a fast SR fades. SL is where decay landed.
	const double sinceSustain = sinceDecay - op.decay;
	op.envelope = std::max (0.0, op.sustainLevel * (1.0 - sinceSustain / std::max (op.sustainRate * 8.0, 1e-6)));
}

} // namespace

double durationOf (const chips::VoicePatch& patch)
{
	Operator ops[4];
	if (! readOperators (patch, ops))
	{
		return 0.0;
	}

	double longest = 0.0;

	for (const auto& op : ops)
	{
		// Attack to the sustain level, then a short hold, then the release. Eight times the
		// sustain sweep is arbitrary but it keeps a fast SR audible and a slow one from making a
		// buffer minutes long.
		const double hold = std::min (op.sustainRate * 8.0, 1.2);
		longest = std::max (longest, op.attack + op.decay + hold + op.release);
	}

	return longest;
}

std::vector<float> render (const chips::VoicePatch& patch, double frequencyHz,
						   double seconds, double rate)
{
	Operator ops[4];
	if (! readOperators (patch, ops))
	{
		return {};
	}

	if (seconds <= 0.0)
	{
		seconds = durationOf (patch);
	}

	const int count = std::max (2, (int) std::lround (seconds * rate));

	const int algorithm = (int) (regAt (patch, (uint8_t) (0xB0 | (patch.regs.empty () ? 0 : (patch.regs.front ().first & 0x03)))) & 0x07);
	const Algorithm& wiring = kAlgorithms[std::clamp (algorithm, 0, 7)];

	const double dt = 1.0 / rate;

	// **The release starts exactly one release-length before the end**, so the tail *is* the
	// release rather than whatever was left at 70%. Keyed off a fraction of the window, a slow
	// release was still falling when the buffer ended and a fast one had been silent for a third
	// of the note - neither of which is what the envelope says happens.
	double longestRelease = 0.0;
	for (const auto& op : ops)
	{
		longestRelease = std::max (longestRelease, op.release);
	}

	const double releaseAt = std::max (0.0, seconds - longestRelease);

	std::vector<float> out ((size_t) count, 0.0f);

	double sinceOn = 0.0;

	for (int i = 0; i < count; ++i)
	{
		const double t = (double) i * dt;
		const bool releasing = t >= releaseAt;

		double mixed = 0.0;

		// **Time advances once per sample, not once per operator.** It used to be advanced inside
		// `advanceEnvelope`, so four operators four times a sample ran the envelope at 4x - the
		// note was over by a quarter of its length, which the "made sound in the middle" check
		// caught as silence.
		sinceOn += dt;

		for (int op = 0; op < 4; ++op)
		{
			Operator& o = ops[op];
			advanceEnvelope (o, releasing, sinceOn, dt);

			// A modulator that reached zero silences what it modulates, which is how the chip
			// behaves and what makes the tail fall away instead of ringing.
			double modulation = 0.0;
			if (wiring.modulate[op] >= 0)
			{
				const Operator& source = ops[wiring.modulate[op]];
				modulation = source.envelope * std::sin (source.phase) * kTwoPi;
			}

			o.phase += kTwoPi * frequencyHz * o.multiple * dt;

			const double value = o.envelope * o.level * std::sin (o.phase + modulation);

			if (wiring.carrier[op])
			{
				mixed += value;
			}
		}

		out[(size_t) i] = (float) std::clamp (mixed * 0.25, -1.0, 1.0);
	}

	return out;
}

} // namespace toybox::chips::noteWave
