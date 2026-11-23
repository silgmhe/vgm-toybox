#include "NoteWaveform.h"

#include <algorithm>
#include <cmath>

#include "VoiceDriver.h"

namespace toybox::playback
{
namespace noteWave
{

double durationOf (const chips::VoicePatch& patch)
{
	(void) patch;

	// The two fallbacks added together, in the same place `render()` finds them.
	return chips::kDefaultSustainSeconds + chips::kDefaultReleaseSeconds;
}

std::vector<float> render (IVoiceDriver& driver, const chips::VoicePatch& patch, double frequencyHz,
						   double sustainSeconds, double releaseSeconds, bool normalise)
{
	// **A one-shot has no frequency, and that is not a reason to refuse it.** The rhythm voices are
	// started by a trigger bit; the pitched guard applies to everything else.
	if (patch.regs.empty () || (frequencyHz <= 0.0 && ! patch.oneShot))
	{
		return {};
	}

	// **The driver decides whether it can play this patch.** An OPN patch handed to an OPLL has no
	// meaning, and a driver that cannot start its core has nothing to give; both say so here rather
	// than returning a buffer of silence that would be drawn as a flat line.
	if (! driver.load (patch))
	{
		return {};
	}

	// **Two stages, both given, neither read from the patch.** The registers are honest and
	// unusable: a release code of 1 is a sweep of four minutes on this chip, and the sustain is a
	// level the envelope decays *through* rather than holds at, so it has no duration to read at
	// all. How long to hold and how long a tail to keep are choices - the caller brings them, and
	// the defaults stand in when it brings nothing.
	// **A drum brings its own length and needs no tail**: the chip's rhythm envelope decays by itself,
	// and a "release" after it would only render silence. The caller's settings describe a held note.
	if (patch.oneShot)
	{
		sustainSeconds = patch.lengthSeconds > 0.0 ? patch.lengthSeconds
												   : chips::kDefaultSustainSeconds;
		releaseSeconds = 0.0;
	}
	else if (sustainSeconds <= 0.0)
	{
		sustainSeconds = chips::kDefaultSustainSeconds;
	}

	if (releaseSeconds <= 0.0 && ! patch.oneShot)
	{
		releaseSeconds = chips::kDefaultReleaseSeconds;
	}

	const double rate = driver.rate ();

	if (rate <= 0.0)
	{
		return {};
	}

	const double total = sustainSeconds + releaseSeconds;
	const int cap = (int) std::lround (std::max (0.05, total) * rate);

	// Key off exactly where the sustain ends, so the tail is the release the caller asked for.
	const int releaseAt = std::min (cap - 1, (int) std::lround (sustainSeconds * rate));

	driver.noteOn (frequencyHz);

	std::vector<float> out ((size_t) cap, 0.0f);

	int done = 0;
	bool released = false;

	while (done < cap)
	{
		if (! released && done >= releaseAt)
		{
			driver.noteOff ();
			released = true;
		}

		// A chunk at a time rather than one sample at a time: a core that renders in blocks gets to
		// do so, and the loop overhead disappears against the cost of the chip.
		const int count = std::min (512, cap - done);
		driver.render (out.data () + done, count);
		done += count;
	}

	// **Normalised to its own peak.** The absolute level a patch produces is meaningless outside the
	// song: a carrier at total level 48 is -36 dB by design, and a modulator at 0x7F is inaudible.
	// Drawn at that level the window shows a flat line, and the exported file is a whisper - both
	// correct and both useless. The shape is what is being shown, so the shape is what fills the
	// range; the relative levels between the operators, which is what makes the timbre, are
	// untouched by a single gain.
	if (normalise)
	{
		float peak = 0.0f;
		for (float sample : out)
		{
			peak = std::max (peak, std::abs (sample));
		}

		if (peak > 1.0e-6f)
		{
			const float gain = 1.0f / peak;

			for (auto& sample : out)
			{
				sample *= gain;
			}
		}
	}

	// **Quantised to the chip's own resolution, not to a number picked by eye.** `fmopn` sums four
	// operators into about `+/-0x1000`, so its samples carry fourteen bits of magnitude; an earlier
	// version quantised to eight on the reasoning that an eight-bit *converter* is the chip's
	// signature, but the DAC's width is not the core's, and discarding six bits the hardware
	// produced is a different sound rather than a more authentic one.
	//
	// `2^(bits-1) - 1` for whatever this patch's chip accumulates, applied last so full scale stays
	// full scale.
	//
	// **The width comes from the driver, not from the patch.** Both would name the same model today,
	// but the driver is the one that scaled the samples by it - asking it is what keeps the scale and
	// the quantiser from drifting into two values.
	// The upper bound is 24, not 16, because one chip needs more than sixteen - see `fmOutputBits`.
	const int bits = driver.noteWidth ();
	const float quantiseSteps = (float) ((1 << (std::clamp (bits, 2, 24) - 1)) - 1);

	for (auto& sample : out)
	{
		sample = std::round (sample * quantiseSteps) / quantiseSteps;
	}

	return out;
}

} // namespace noteWave
} // namespace toybox::playback
