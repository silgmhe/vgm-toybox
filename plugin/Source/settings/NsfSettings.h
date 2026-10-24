#pragma once

namespace toybox::settings::nsf
{

/** How long an NSF plays, in seconds.

	An NSF has no length: its driver loops until it is stopped, so this is the number the timeline,
	the seek bar and the end of a song are built on.

	The store clamps to these and the settings page draws its slider from them. They live together
	because a slider that can reach a value the store refuses snaps back without saying anything -
	which reads as a broken slider, not as a mismatch between two files. */
constexpr int minDurationSeconds = 10;
constexpr int maxDurationSeconds = 600;
constexpr int durationStepSeconds = 5;
constexpr int defaultDurationSeconds = 180;

/** Output gain in decibels.

	The NES mix table leaves a lot of headroom - five four-bit channels never approach full scale -
	so a file played unamplified sits eight to twelve decibels below a VGM of the same music. Eight
	is enough to bring the corpus level; the range goes to twenty-four so a quiet file can be lifted
	without leaving the page, at the cost of clipping the loudest ones. */
constexpr int minOutputGainDb = 0;
constexpr int maxOutputGainDb = 24;
constexpr int outputGainStepDb = 1;
constexpr int defaultOutputGainDb = 8;

} // namespace toybox::settings::nsf
