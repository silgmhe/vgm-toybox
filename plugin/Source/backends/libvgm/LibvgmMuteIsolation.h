#pragma once

#include "stdtype.h"
#include "player/playerbase.hpp"

namespace toybox::backends::libvgm
{

/** Channel number -> position of its mute bit inside PLR_MUTE_OPTS::chnMute[2] (32 bits each). */
inline int channelMuteHalf (int channelIndex) { return channelIndex / 32; }
inline UINT32 channelMuteBit (int channelIndex) { return 1u << (channelIndex % 32); }

/** Suspends a device completely. `disable` skips the emulation, which is what makes a dozen
	isolated players affordable - the devices a track does not need cost no CPU at all. */
inline PLR_MUTE_OPTS suspendedMuteOptions (void)
{
	PLR_MUTE_OPTS opts = {};
	opts.disable = 0xFF;
	opts.chnMute[0] = 0xFFFFFFFF;
	opts.chnMute[1] = 0xFFFFFFFF;
	return opts;
}

/** Keeps the whole device audible (every channel of it). */
inline PLR_MUTE_OPTS wholeDeviceMuteOptions (void)
{
	PLR_MUTE_OPTS opts = {};
	opts.disable = 0x00;
	opts.chnMute[0] = 0x00;
	opts.chnMute[1] = 0x00;
	return opts;
}

/** Keeps exactly one channel of the device audible; `channelIndex < 0` (or a device whose core
	cannot mute single channels) leaves the whole device audible. */
inline PLR_MUTE_OPTS singleChannelMuteOptions (int channelIndex, bool isolatable)
{
	PLR_MUTE_OPTS opts = {};
	opts.disable = 0x00;

	for (int half = 0; half < 2; ++half)
	{
		UINT32 mask = 0x00;
		if (isolatable && channelIndex >= 0)
		{
			mask = 0xFFFFFFFF;
			if (channelMuteHalf (channelIndex) == half)
			{
				mask &= ~channelMuteBit (channelIndex);
			}
		}
		opts.chnMute[half] = mask;
	}

	return opts;
}

/** Mutes one device through the chips card. Mixer tracks mute the main device (`0x01`, the
	linked devices are suspended on their own), the plain player mutes main + linked (`0x03`). */
inline PLR_MUTE_OPTS chipMuteOptions (bool muted, bool isolatedPlayer)
{
	PLR_MUTE_OPTS opts = wholeDeviceMuteOptions();
	opts.disable = muted ? (isolatedPlayer ? 0x01 : 0x03) : 0x00;
	return opts;
}

} // namespace toybox::backends::libvgm
