#pragma once

#include <cstddef>
#include <string>

#include "midi/SongIr.h"

namespace toybox::formats::vgm
{

/** Reads a VGM/VGZ image into the format-neutral SongIr.

	This is the only place that knows what a VGM command number means. It answers three questions and
	nothing else: which chip a write belongs to, where in that chip it lands, and when. It never
	decides what a note is, never touches libvgm and never sees a MIDI message - which is what makes
	a second loader (an NSF one, say) a peer of this file rather than a rewrite of the assembly layer.

	Device clocks are not filled in here: a VGM header names them, but so does the runtime's device
	list, and the runtime is the one that starts the chips. The assembly layer fills `clockHz` from
	`PLR_DEV_INFO`; `flags` carries the few variants a chip needs (C140's banking, RF5C164 vs 68). */
class VgmSongLoader final : public midi::ISongLoader
{
public:
	bool load (const void* data, std::size_t size, midi::SongIr& out, std::string& error) override;
};

} // namespace toybox::formats::vgm
