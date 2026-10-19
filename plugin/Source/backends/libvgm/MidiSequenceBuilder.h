#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "playback/MidiSequence.h"
#include "playback/SongBackend.h"

#include "player/playerbase.hpp"

namespace toybox::backends::libvgm
{

/** Builds the read-only MIDI event stream for a VGM/VGZ image. The image must already be
	uncompressed (LibvgmSongInstance::playerMemory). Returns null when the image is not a VGM
	or no channel has a decoder. */
std::unique_ptr<playback::MidiSequence> buildMidiSequence (const void* data, std::size_t size,
														   const std::vector<PLR_DEV_INFO>& devices,
														   const std::vector<playback::TrackInfo>& tracks);

} // namespace toybox::backends::libvgm
