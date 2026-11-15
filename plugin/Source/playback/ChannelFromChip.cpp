#include "ChannelFromChip.h"

#include <cmath>
#include <utility>

#include "WaveShape.h"

namespace toybox::playback
{

int waveKindForChannelKind (chips::ChannelKind kind)
{
	using chips::ChannelFamily;
	using Kind = waveform::Kind;

	// **The one kind whose drawn shape is not its family.** The DMC classifies as `pcm` for the mark -
	// one bit is still a recording - and draws as `delta`, the staircase it actually makes. Everything
	// else falls out of `familyOf`, which is what keeps this function from being a second table.
	if (kind == chips::ChannelKind::nesDmc)
	{
		return (int) Kind::delta;
	}

	switch (chips::familyOf (kind))
	{
	case ChannelFamily::pulse:		return (int) Kind::pulse;
	case ChannelFamily::triangle:	return (int) Kind::triangle;
	case ChannelFamily::saw:		return (int) Kind::saw;
	case ChannelFamily::noise:		return (int) Kind::noise;
	case ChannelFamily::fm:			return (int) Kind::fm;

	// Both read their shape out of memory, so both draw as `stored` - a chip whose decoder cannot
	// hand a cycle over draws a placeholder rather than a lie (design document, section 3.1).
	case ChannelFamily::wavetable:
	case ChannelFamily::pcm:		return (int) Kind::stored;

	case ChannelFamily::none:		break;
	}

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
	state.channelKind = hints.channelKind;

	// Both travel as they are: the decoder already owns the wave behind a shared pointer, so this
	// is a reference count rather than a copy, and a rate is a value the chip states.
	state.wave = chipState.wave;
	state.sampleRate = chipState.sampleRate;

	// A rate says how fast a thing is read, not what kind of thing it is: a wavetable walks its
	// table per second too, and the export has to know which of the two it is holding.
	state.tableWave = chipState.tableWave;

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
	else if (chipState.hasPitch || chipState.period != 0 || chipState.block != 0)
	{
		int finePitch = 0;
		const int note = noteTable.noteFor (chipState.period, chipState.block, &finePitch);

		if (note >= 0)
		{
			state.frequency = (float) (440.0 * std::pow (2.0, (note - 69 + finePitch / 100.0) / 12.0));
		}
	}

	state.note = noteForFrequency (state.frequency);

	// A percussion channel whose pitch cannot be read keeps the note a drum part is written at.
	// This is the same answer the MIDI path gives these channels - `describeSamplePitch` returns no
	// layers for one and says the caller reports the C4 trigger - and it is what lets a sample row
	// take part in the note grid instead of standing outside it with an empty cell.
	//
	// Only when the pitch is genuinely missing. A noise channel is percussion too, but its period
	// is a real number and the row shows the note it makes, which is why this asks about the empty
	// frequency rather than about `isDrum`.
	if (state.note < 0 && hints.isDrum)
	{
		state.note = 60;
		state.frequency = 261.63f;
	}

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
