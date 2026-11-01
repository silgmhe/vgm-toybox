#pragma once

#include <cstddef>
#include <vector>

namespace toybox::playback::waveform
{

/** How a channel makes its shape, as the backend describes it (see playback/ChipState.h).

	The panel draws whatever arrives; this function is the fallback for the channels that generate
	a shape rather than read one from memory. Keeping it here means the drawing code has no case
	statement per chip, and that the shapes can be checked without rendering anything.
*/
enum class Kind
{
	none = 0,		// nothing to draw
	stored = 1,		// the backend supplied `ChannelState::wave`
	pulse = 2,
	triangle = 3,
	noise = 4,
	delta = 5,
	saw = 6,		// the VRC6's ramp: a counter that climbs until it overflows
	fm = 7			// the VRC7: two operators, a carrier and a modulator
};

/** One cycle of the shape, values in -1..1, or empty when there is nothing to synthesise.

	`duty` is the pulse width as a fraction of the period and is ignored by the other kinds.

	`shapeParameter` is what the noise and sample kinds need beyond their kind: the noise channel's
	period, or a DPCM sample's identity. For `Kind::fm` it carries the four numbers of a two-operator
	voice, packed by packFm() - one int rather than four new fields, because this value already
	travels through the snapshot, the row signature and the exporter, and each of those would
	otherwise have to learn to compare four more.
*/
std::vector<float> cycleFor (int waveKind, float duty, int shapeParameter = 0);

/** The four numbers a two-operator FM voice needs, packed into one int.

	They come from the emulated chip's own patch: the modulator's frequency multiple and total
	level, the carrier's waveform select and the carrier's feedback. Layout, low bits first:

		bits 0-3    multiple, 0..15   (a table, not a ratio: 0 is half speed)
		bits 4-9    level, 0..63      (attenuation, 0 is loudest)
		bits 10-11  waveform, 0..3    (sine, half, absolute, pulse)
		bits 12-14  feedback, 0..7

	Fifteen bits of a thirty-two-bit int, which leaves room for the two reserved bits to be used
	without moving anything.
*/
constexpr int packFm (int multiple, int level, int waveform, int feedback)
{
	return ((multiple & 0xF) | ((level & 0x3F) << 4) | ((waveform & 0x3) << 10) | ((feedback & 0x7) << 12));
}

constexpr int fmMultiple (int packed) { return packed & 0xF; }
constexpr int fmLevel (int packed) { return (packed >> 4) & 0x3F; }
constexpr int fmWaveform (int packed) { return (packed >> 10) & 0x3; }
constexpr int fmFeedback (int packed) { return (packed >> 12) & 0x7; }

/** One column of a drawn wave: the quietest and the loudest point in its slice.

	A display has a hundred-odd columns for a sample of tens of thousands of points, and one number
	per column throws away which way the sound swings. A pair is what lets a row be filled between
	the two extremes - the way a sampler draws a sample - instead of being a line through the peaks,
	which for a recorded wave is an envelope drawn thin: it swings the right way and shows none of
	the size.

	Two numbers rather than one also keeps a drum's attack, which is one or two samples wide: a fixed
	stride across the sample would step over it, and two hits that differ only in their first
	millisecond would draw the same shape.
*/
struct Column
{
	float min = 0.0f;
	float max = 0.0f;
};

/** `values` reduced to at most `limit` columns.

	A stored table is at most 256 points, so this only bites on samples: a DPCM hit is up to 32648
	points and a row is about a hundred and twenty pixels wide. The file a channel exports keeps
	every point; this decides only what a row shows.
*/
std::vector<Column> decimateForDisplay (const std::vector<float>& values, std::size_t limit);

} // namespace toybox::playback::waveform
