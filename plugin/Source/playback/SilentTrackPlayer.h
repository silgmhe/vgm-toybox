#pragma once

#include "SongBackend.h"

#include <algorithm>
#include <cstddef>

namespace toybox::playback
{

/**
	**A player for a track that has nothing to play.**

	The backend can tell, from the parsed register stream, that a track never appears in the song -
	on a file with eighty channels of which three sound, that is seventy-seven emulations that would
	exist only to output silence. Returning null for them saved the memory but moved a fact into
	every caller: whether a player exists had to be checked at each place that walks the list, and
	the eight checks were found one crash at a time. The ninth would have been found by a user.

	A player that does nothing is the same answer with no checks. Callers keep the shape they had
	before any of this - every track has a player, always - and there is no chip behind this one, so
	it costs a few bytes rather than a sample ROM.

	`captureState` and `renderNoteAt` keep the interface defaults: a track with no notes has no
	register state to describe and no note to draw, and the "no" they return is already the one the
	panels handle.
*/
class SilentTrackPlayer : public ITrackPlayer
{
public:
	void start (void) override {}
	void pause (void) override {}
	bool seekSeconds (double /*seconds*/) override { return true; }
	void setTimeline (int /*loopCount*/, double /*fadeSeconds*/) override {}
	void setTrackAudible (TrackId /*track*/, bool /*audible*/) override {}
	void setChipAudible (DeviceId /*device*/, bool /*audible*/) override {}

	int render (float* left, float* right, int numSamples) override
	{
		// **Cleared, not skipped.** The mixer accumulates into a shared buffer, so leaving it
		// untouched would add whatever the previous track left there - that is not silence, it is
		// another track's audio played twice.
		if (left != nullptr)
		{
			std::fill (left, left + numSamples, 0.0f);
		}

		if (right != nullptr)
		{
			std::fill (right, right + numSamples, 0.0f);
		}

		return numSamples;
	}

	double positionSeconds (void) override { return 0.0; }

	/** Not finished: a track that never plays must not make the song look over. */
	bool isFinished (void) const override { return false; }
};

} // namespace toybox::playback
