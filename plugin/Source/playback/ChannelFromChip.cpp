#include "ChannelFromChip.h"

#include <cmath>
#include <utility>

#include "WaveShape.h"

namespace toybox::playback
{

int waveKindForChannelKind (chips::ChannelKind kind)
{
	using chips::ChannelKind;
	using Kind = waveform::Kind;

	switch (kind)
	{
		// Square waves. AY's envelope channel is a square wave wearing a hardware volume
		// envelope, so it draws as a pulse - the envelope decides how loud, not what shape.
		case ChannelKind::nesPulse:
		case ChannelKind::gbPulse:
		case ChannelKind::vrc6Pulse:
		case ChannelKind::mmc5Pulse:
		case ChannelKind::aySquare:
		case ChannelKind::ayEnvelope:
		case ChannelKind::sn76489Square:
		case ChannelKind::saaSquare:
		case ChannelKind::pokeySquare:
			return (int) Kind::pulse;

		case ChannelKind::nesTriangle:
			return (int) Kind::triangle;

		// An LFSR: audible, but with no period to name.
		case ChannelKind::nesNoise:
		case ChannelKind::gbNoise:
		case ChannelKind::ayNoise:
		case ChannelKind::sn76489Noise:
		case ChannelKind::wsNoise:
			return (int) Kind::noise;

		case ChannelKind::nesDmc:
			return (int) Kind::delta;

		case ChannelKind::vrc6Saw:
			return (int) Kind::saw;

		// Two operators, so the shape is the patch rather than a kind. The four numbers are
		// packed into `shapeParameter`; see waveform::packFm.
		case ChannelKind::vrc7Fm:
		case ChannelKind::fm:
		case ChannelKind::fmDrum:
			return (int) Kind::fm;

		// Everything that reads its shape from memory. `stored` says "the backend supplied the
		// cycle" - and a chip in this list whose decoder cannot hand one over draws a placeholder
		// rather than a lie (design document, section 3.1).
		case ChannelKind::gbWave:
		case ChannelKind::fdsWave:
		case ChannelKind::n163Wave:
		case ChannelKind::c6280Wave:
		case ChannelKind::sccWave:
		case ChannelKind::x1Wave:
		case ChannelKind::wavetableWave:
		case ChannelKind::wsWave:
		case ChannelKind::rf5cPcm:
		case ChannelKind::c140Pcm:
		case ChannelKind::c352Pcm:
		case ChannelKind::k053260Pcm:
		case ChannelKind::k054539Pcm:
		case ChannelKind::qsoundPcm:
		case ChannelKind::ga20Pcm:
		case ChannelKind::okimAdpcm:
		case ChannelKind::segaPcm:
		case ChannelKind::opnAdpcmB:
		case ChannelKind::adpcmDrum:
			return (int) Kind::stored;

		case ChannelKind::unknown:
			return (int) Kind::none;
	}

	// Only reachable if a decoder reports a kind added to the enum after this switch was written -
	// which the compiler warns about, so it is a defensive answer rather than an expected one.
	return (int) Kind::none;
}

ChannelState channelFromChip (const chips::ChannelState& chipState,
							  const chips::NoteTable& noteTable,
							  const ChipShapeHints& hints,
							  std::string chipName,
							  std::string channelName)
{
	ChannelState state;

	state.chipName = std::move (chipName);
	state.name = std::move (channelName);
	state.waveKind = hints.waveKind;
	state.duty = hints.duty;

	// Both travel as they are: the decoder already owns the wave behind a shared pointer, so this
	// is a reference count rather than a copy, and a rate is a value the chip states.
	state.wave = chipState.wave;
	state.sampleRate = chipState.sampleRate;

	if (hints.patchIsDuty)
	{
		// Four widths, in the order the chips number them: 12.5, 25, 50 and 75 percent. A patch
		// outside the range comes from a decoder that read a register the chip does not have; the
		// middle width is the least misleading answer to that.
		static constexpr float kDutyOfPatch[4] = { 0.125f, 0.25f, 0.5f, 0.75f };
		state.duty = kDutyOfPatch[(chipState.patch < 0 || chipState.patch > 3) ? 2 : chipState.patch];
	}

	if (! chipState.present)
	{
		return state;		// a channel the chip does not have: no name to give it either
	}

	// Key off but still audible counts as active: an FM voice releasing over half a second would
	// otherwise blink out the instant the key came up, which reads as a dropped frame.
	state.active = chipState.gate || chipState.released;

	// The decoder fills one of the two pitch camps and never both (chips/ChipTypes.h, PitchFields).
	// The period camp is the one that needs the chip's own table, because a period is not a
	// frequency until the chip's divider says what it divides.
	if (chipState.hz > 0.0)
	{
		state.frequency = (float) chipState.hz;
	}
	else if (chipState.period != 0 || chipState.block != 0)
	{
		int finePitch = 0;
		const int note = noteTable.noteFor (chipState.period, chipState.block, &finePitch);

		if (note >= 0)
		{
			state.frequency = (float) (440.0 * std::pow (2.0, (note - 69 + finePitch / 100.0) / 12.0));
		}
	}

	state.note = noteForFrequency (state.frequency);

	// `volume` arrives in the chip's own units and leaves normalised, because the row draws it as
	// a bar of unknown length otherwise.
	if (hints.volumeMaximum > 0)
	{
		const float filled = (float) chipState.volume / (float) hints.volumeMaximum;
		state.volume = filled < 0.0f ? 0.0f : (filled > 1.0f ? 1.0f : filled);
	}

	// `patch` means a different thing on every chip family - a duty on a pulse channel, a sample
	// number on a PCM one - so only the kinds that read it as one number get it. The chips whose
	// patch is a duty say so through `hints.duty`; the rest carry it as the shape's parameter.
	if (hints.waveKind == (int) waveform::Kind::fm)
	{
		// Packed only when the decoder says it read the patch. An FM channel whose operator
		// registers were never parsed would otherwise be drawn as "multiple 0, level 0, no
		// feedback" - a real patch that belongs to some other sound. Zero leaves cycleFor() on its
		// generic sine, which is the honest answer to "this one is not known".
		state.shapeParameter = chipState.fmKnown
								   ? waveform::packFm (chipState.fmMultiple, chipState.fmLevel,
													   chipState.fmWaveform, chipState.fmFeedback)
								   : 0;
	}
	else if (hints.waveKind == (int) waveform::Kind::noise
			 || hints.waveKind == (int) waveform::Kind::stored
			 || hints.waveKind == (int) waveform::Kind::delta)
	{
		state.shapeParameter = chipState.patch;
	}

	return state;
}

} // namespace toybox::playback
