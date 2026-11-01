#pragma once

namespace toybox::settings::exporting
{

/** How long a wavetable dragged out of the window stays in the cache, in hours.

	Off by default, and the defaults are the point: deleting a file the user made is worse than
	keeping a few megabytes, so the cache only clears when someone has asked it to. A file dragged
	out but not yet imported is exactly what a cleaner would eat first.

	One hour is the floor because a lower number is a cleaner eating the file the user is dragging.
	A week is the ceiling because past that the number stops describing a cache.
*/
constexpr int minRetentionHours = 1;
constexpr int maxRetentionHours = 168;
constexpr int defaultRetentionHours = 3;

/** Whether the cache is cleared on start-up at all. Default off - see above. */
constexpr bool defaultCleanupEnabled = false;

} // namespace toybox::settings::exporting
