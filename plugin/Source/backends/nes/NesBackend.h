#pragma once

#include <cstddef>
#include <memory>

#include <juce_core/juce_core.h>

#include "playback/SongBackend.h"

namespace toybox::backends::nes
{

/** True when the image is a FamiTracker module, judged by its magic rather than by its name. */
bool isFtmImage (const void* data, std::size_t size);

/** The playback backend for modules: it compiles the file into register writes and drives built-in
	chip cores with them, so a module plays every voice it uses - including the expansion chips that
	the VGM route cannot express at all. */
class NesBackend final : public playback::ISongBackend
{
public:
	juce::String getName (void) const override;
	juce::StringArray getFileExtensions (void) const override;
	bool canOpen (const playback::SongMemory& memory) const override;
	std::unique_ptr<playback::ISongInstance> open (const playback::SongMemory& memory,
												   juce::String& errorMessage) override;
};

} // namespace toybox::backends::nes
