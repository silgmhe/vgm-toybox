#pragma once

#include <vector>

#include <juce_core/juce_core.h>

#include "stdtype.h"
#include "player/playerbase.hpp"

namespace vgmplay
{

/** One mixer track: a single channel of one sound device of the loaded song. */
struct TrackLayoutEntry
{
	UINT32 deviceID = 0;		// PLR_DEV_ID() value of the device - the key for SetDeviceMuting()
	int deviceIndex = 0;		// device order in the song (tracks sharing it form one group)
	int channelIndex = 0;		// channel number inside the device (0-based)
	juce::String name;			// channel name from the core ("Pulse 1", ...) or "Ch n" as fallback
	juce::String groupName;		// group header text (device name, "#2" when the type repeats)
};

/** Channel number -> position of its mute bit inside PLR_MUTE_OPTS::chnMute[2] (32 bits each). */
inline int channelMuteHalf (int channelIndex) { return channelIndex / 32; }
inline UINT32 channelMuteBit (int channelIndex) { return 1u << (channelIndex % 32); }

/** Builds one track per channel of every device the song uses.

	Linked devices are driven through their parent's options and are therefore not tracks of
	their own. Note that unlike the chips card this keeps a T6W28's second half: the card hides
	it because it would only repeat the group, the mixer must not - its channels carry audio. */
std::vector<TrackLayoutEntry> buildTrackLayout (const PlayerBase& player);

} // namespace vgmplay
