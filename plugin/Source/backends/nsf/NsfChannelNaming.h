#pragma once

#include <string>

namespace toybox::backends::nsf
{

/** The N163's eight channels. */
inline constexpr int kN163Channels = 8;

/** The name a composer would use for one N163 channel.

	The chip numbers its channels 7 down to 0, so index 0 - the first the API reports - is channel
	8. The mixer's strip and the keyboard's row both take their name from here: they disagreed once,
	and the same song read "Wave 8" in one card and "Wave 1" in the other.
*/
inline std::string n163ChannelName (int index)
{
	return "Wave " + std::to_string (kN163Channels - index);
}

} // namespace toybox::backends::nsf
