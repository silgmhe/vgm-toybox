#pragma once

#include <vector>

namespace toybox::backends::libvgm
{

/** **Where each device's channels begin in the track numbering.**

    The engine numbers tracks device by device: the first device owns `0 .. n-1`, and every device
    after it continues from the end of the previous block. `describeSong` does that arithmetic when it
    creates the tracks; the snapshot needs the same numbers to attach a state to the right row, and
    the two used to compute them separately.

    Keeping the arithmetic here, as a function of the block sizes alone, is what removes the guess: a
    device is placed by the sizes that came before it, never by its id, its pointer or the order some
    container happened to hand it over in. The YM2610 is the case that forced it - thirteen voices
    followed by a linked three-voice SSG that libvgm reports with its parent's id and `PLR_DEV_INFO`,
    so nothing about the device could tell the two apart.

    `channelCounts` is one entry per device, in the order they are walked. A zero or negative count
    contributes nothing, which is what an empty device should do rather than shift everything after it.
*/
inline std::vector<int> channelBases (const std::vector<int>& channelCounts)
{
	std::vector<int> bases;
	bases.reserve (channelCounts.size ());

	int next = 0;

	for (const int count : channelCounts)
	{
		bases.push_back (next);

		if (count > 0)
		{
			next += count;
		}
	}

	return bases;
}

} // namespace toybox::backends::libvgm
