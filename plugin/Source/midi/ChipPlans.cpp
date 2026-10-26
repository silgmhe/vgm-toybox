#include "ChipPlans.h"

#include <array>
#include <cstddef>

namespace toybox::midi
{

using playback::MidiCapability;

namespace
{
	/** Indexed by ChipKind, in declaration order.

		The static_assert below fails the moment a kind is added at the end without a plan here -
		which is the whole point: the table and the vocabulary are one list now, and the compiler
		notices when they drift apart. Before this, capability lived in a switch keyed on libvgm's
		device ids, and adding a decoder there was a silent no-op.
	*/
	constexpr std::array<ChannelPlan, 41> kPlans
	{{
		/* unknown     */ { 0,  0, 0 },
		/* ay8910      */ { 3,  0, 0 },	// three squares; noise and envelope feed them
		/* sn76489     */ { 3,  1, 0 },	// three tones, then the noise generator's trigger
		/* saa1099     */ { 6,  0, 0 },
		/* pokey       */ { 4,  0, 0 },
		/* ym2413      */ { 9,  5, 0 },	// nine FM, five rhythm
		/* ym3812      */ { 9,  5, 0 },
		/* ym3526      */ { 9,  5, 0 },
		/* y8950       */ { 9,  6, 0 },	// nine FM, five rhythm, Delta-T
		/* ymf262      */ { 18, 5, 0 },
		/* ymf278b     */ { 18, 5, 0 },	// the OPL3 half; the wavetable half is not decoded
		/* ym2612      */ { 6,  1, 0 },	// six FM, then the DAC's trigger
		/* ym2203      */ { 3,  0, 3 },	// three FM, then its linked SSG
		/* ym2608      */ { 6,  6, 4 },	// six FM, six ADPCM-A triggers, ADPCM-B + SSG
		/* ym2610      */ { 6,  6, 4 },	// the same, minus channels 0 and 3 on the non-B part
		/* ym2151      */ { 8,  0, 0 },
		/* ymf271      */ { 12, 0, 0 },
		/* ymz280b     */ { 0,  8, 0 },	// a playback rate, not a note: triggers only
		/* c140        */ { 24, 0, 0 },
		/* c352        */ { 32, 0, 0 },
		/* es5503      */ { 32, 0, 0 },
		/* ga20        */ { 4,  0, 0 },
		/* k053260     */ { 4,  0, 0 },
		/* k054539     */ { 8,  0, 0 },
		/* multiPcm    */ { 28, 0, 0 },
		/* okim6295    */ { 4,  0, 0 },
		/* qsound      */ { 19, 0, 0 },	// sixteen sampled voices plus three ADPCM
		/* rf5c68      */ { 8,  0, 0 },
		/* rf5c164     */ { 8,  0, 0 },
		/* segapcm     */ { 16, 0, 0 },
		/* gameBoy     */ { 3,  1, 0 },	// three tone channels, then the noise channel
		/* huc6280     */ { 6,  0, 0 },
		/* k005289     */ { 2,  0, 0 },
		/* k007232     */ { 2,  0, 0 },
		/* k051649     */ { 5,  0, 0 },
		/* mikey       */ { 4,  0, 0 },
		/* msm5232     */ { 8,  0, 0 },
		/* nesApu      */ { 3,  2, 0 },	// two pulses and the triangle, then noise and DPCM
		/* vsu         */ { 6,  0, 0 },	// five wavetable voices and one noise voice
		/* wonderswan  */ { 4,  0, 0 },
		/* x1_010      */ { 16, 0, 0 },
	}};

	static_assert (kPlans.size() == static_cast<std::size_t> (ChipKind::x1_010) + 1,
				   "ChipKind gained a value: add its plan to kPlans, and the coverage check in "
				   "ChipPlansTest will then hold it to what its decoder actually produces");
}

MidiCapability ChannelPlan::at (int index) const
{
	if (index < 0)
	{
		return MidiCapability::unavailable;
	}

	if (index < melodicCount)
	{
		return MidiCapability::melodic;
	}

	if (index < melodicCount + oneShotCount)
	{
		return MidiCapability::oneShot;
	}

	if (index < melodicCount + oneShotCount + tailMelodicCount)
	{
		return MidiCapability::melodic;
	}

	return MidiCapability::unavailable;
}

ChannelPlan channelPlanFor (ChipKind kind)
{
	const auto index = static_cast<std::size_t> (kind);

	if (index >= kPlans.size ())
	{
		return {};
	}

	return kPlans[index];
}

} // namespace toybox::midi
