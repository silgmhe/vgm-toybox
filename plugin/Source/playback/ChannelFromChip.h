#pragma once

#include <string>

#include "chips/ChipTypes.h"

#include "ChipState.h"

namespace toybox::playback
{

/** What a panel needs to know about one chip channel before any register is read.

	Written once per chip, not per frame: the shape a channel makes and the scale its volume
	register uses are properties of the chip. Everything that changes while the song plays comes
	from `chips::ChannelState`.

	It exists so the translation below has no `switch` over chip kinds. Thirty decoders would
	otherwise earn thirty branches in one function, and adding a chip would mean editing it - which
	is the shape this project keeps trying to avoid (rule 4).
*/
struct ChipShapeHints
{
	/** `waveform::Kind` as an int, because that is what `ChannelState::waveKind` carries. */
	int waveKind = 0;

	/** Ignored by every kind except `pulse`. */
	float duty = 0.5f;

	/** Whether `patch` is a duty index rather than a waveform number.

		True for the chips whose pulse channel selects one of four widths by a two-bit register -
		the 2A03, the DMG, the VRC6 and the MMC5 - and false for the ones that are always square
		(AY, SAA1099, POKEY) or whose patch means something else entirely. The four widths are
		12.5, 25, 50 and 75 percent, in the order every one of those chips numbers them. */
	bool patchIsDuty = false;

	/** The largest value `chips::ChannelState::volume` can hold on this chip. Most are 4-bit and
		use 15; a chip with a wider attenuation register says so here rather than in the decoder. */
	int volumeMaximum = 15;
};

/** The shape a channel kind makes, in `waveform::Kind` terms.

	A `switch` and not a table on purpose: the compiler warns when an enumerator is not handled, so
	a chip added to `ChannelKind` cannot be silently left out of the drawing code. A lookup table
	misses it and nothing says so.
*/
int waveKindForChannelKind (chips::ChannelKind kind);

/** One channel's state, translated for display.

	Two structs share the name `ChannelState` and have no field in common (see the design document,
	section 1). This is the only place that knows both.

	@param chipState what the decoder says, in register terms
	@param noteTable the chip's own period-to-note mapping; only consulted when the decoder filled
	                 `period`/`block` rather than `hz`, because a period means different pitches on
	                 a CPU divider and on an FM chip
	@param hints      the chip's fixed properties
	@param chipName   shown in the row and carried into an exported file's name
	@param channelName likewise
*/
ChannelState channelFromChip (const chips::ChannelState& chipState,
							  const chips::NoteTable& noteTable,
							  const ChipShapeHints& hints,
							  std::string chipName,
							  std::string channelName);

} // namespace toybox::playback
