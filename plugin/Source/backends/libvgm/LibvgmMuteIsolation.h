#pragma once

#include "stdtype.h"
#include "emu/SoundDevs.h"
#include "player/playerbase.hpp"

namespace toybox::backends::libvgm
{

/** What the device linked behind a chip is, relative to that chip.

	The distinction decides whether one mute mask can serve both halves of a chain:

	- `mirror`     the linked device is an implementation detail of the parent and its channel n is
	               the parent's channel n - an OPL4 is a YMF278B with an OPL3 doing its FM. The same
	               mask belongs in both halves, and muting the linked device silences the chip.
	- `extraVoices` the linked device is a voice set of its own with its own numbering - the SSG
	               behind an OPN chip is three channels behind three, thirteen or thirteen more.
	               The parent's mask means something else there, so the linked device must be kept
	               out of a parent track's render entirely.

	Only four devices in libvgm have a linked device at all (YM2203, YM2608, YM2610/B behind an
	SSG; YMF278B behind an OPL3), which is why this can be a type test rather than a table.
*/
enum class LinkedDeviceKind
{
	mirror,
	extraVoices
};

inline LinkedDeviceKind linkedDeviceKindFor (DEV_ID type)
{
	switch (type)
	{
	case DEVID_AY8910:		// SSG: AY-3-8910 and its clones, three channels of its own
		return LinkedDeviceKind::extraVoices;

	default:
		return LinkedDeviceKind::mirror;
	}
}

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
	cannot mute single channels) leaves the whole device audible.

	Both halves carry that same mask: libvgm applies `chnMute[1]` to the *linked* device
	(VGMPlayer::RefreshMuting), and a linked device describes the same channels as its parent -
	an OPL4's FM half is an OPL3 whose channel n is the parent's channel n. Filling the second
	half with "everything else muted" - the natural reading of a two-word mask - used to mute
	exactly the channel the track was about, which is why an OPL4 had no audio on its tracks
	while the plain player played it. */
inline PLR_MUTE_OPTS singleChannelMuteOptions (int channelIndex, bool isolatable)
{
	PLR_MUTE_OPTS opts = {};
	opts.disable = 0x00;

	UINT32 mask = 0x00;
	if (isolatable && channelIndex >= 0)
	{
		mask = 0xFFFFFFFF & ~channelMuteBit (channelIndex);
	}

	// Both halves carry the same mask: libvgm applies chnMute[1] to the *linked* device
	// (VGMPlayer::RefreshMuting), and when a chip's linked device describes the same channels as
	// its parent - an OPL4's FM half is an OPL3 whose channel n is the parent's channel n - the
	// track's own channel has to stay audible in that half too. Filling it with "everything
	// muted" is the natural reading of a two-word mask and it silences exactly the tracks the
	// mixer is trying to isolate.
	//
	// A linked device with channel numbers of its own does not fit this, and cannot be fixed
	// here: see the YM2610 case in LibvgmTrackPlayer::applyIsolation, where the linked SSG no
	// longer takes part in a track's render at all.
	opts.chnMute[0] = mask;
	opts.chnMute[1] = mask;

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
