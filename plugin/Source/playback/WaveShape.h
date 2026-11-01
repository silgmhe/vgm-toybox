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

/** `values` reduced to at most `limit` points, keeping the extreme of each bucket.

	The extreme rather than every nth value: a drum's attack is one or two samples wide, and a fixed
	stride drops it - two hits differing only in their first millisecond would draw the same line.
	The value furthest from zero in each bucket keeps the spike that tells them apart.

	A stored table is at most 256 points, so this only bites on samples: a DPCM hit is up to 32648
	points and a row is about a hundred and twenty pixels wide, where that many segments overlap
	into a smear. The file a channel exports keeps every point; this decides only what a row shows.
*/
std::vector<float> decimateForDisplay (const std::vector<float>& values, std::size_t limit);

	/** The same, over the first `count` points only.

		A recorded sample can be thousands of points long while a row has room for a hundred or so.
		Downsampling all of it by peak draws the sample's envelope - a shape that rises and falls and
		contains no period at all. One cycle is what a waveform view is a picture of, and this is how
		a caller says where one ends. Nothing is copied but the result.
	*/
	std::vector<float> decimateForDisplay (const std::vector<float>& values, std::size_t limit,
										   std::size_t count, std::size_t start = 0);

	/** Where the loudest window of `count` points begins, or zero when the whole sample fits.

		A cycle taken from the front of a sample is often its silence: an instrument's attack is not
		always at offset zero, and a C140 voice's start address is where the driver pointed, not
		where the sound begins. Rows drawn from there come out as a flat line while the channel is
		plainly playing.

		Windows are stepped without overlap, which makes this one pass over the sample rather than a
		search, and it runs once per row per frame.
	*/
	std::size_t bestWindowStart (const std::vector<float>& values, std::size_t count);

} // namespace toybox::playback::waveform
