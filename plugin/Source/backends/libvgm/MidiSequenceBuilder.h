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
	or no channel has a decoder.

	`transposeBySampleRoot` decides whether a sampled voice's note also carries the root measured
	out of its sample (design doc section 15): off, each note is the transposition the registers ask
	for, which is always computable; on, the sample's own root moves it to the absolute octave. */
std::unique_ptr<playback::MidiSequence> buildMidiSequence (const void* data, std::size_t size,
														   const std::vector<PLR_DEV_INFO>& devices,
														   const std::vector<playback::TrackInfo>& tracks,
														   bool transposeBySampleRoot);

} // namespace toybox::backends::libvgm
