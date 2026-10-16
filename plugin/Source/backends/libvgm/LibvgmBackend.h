#pragma once

#include "playback/SongBackend.h"

namespace toybox::backends::libvgm
{

/**
	The libvgm backend: VGM/VGZ (and S98/DRO/GYM, which libvgm registers along with the VGM
	player - whether the UI advertises them is the registry's business, see
	BackendRegistry::getFileExtensions()).

	The whole coupling to libvgm lives under Source/backends/libvgm/: this class is the only
	entry point the core knows.
*/
class LibvgmBackend final : public playback::ISongBackend
{
public:
	juce::String getName (void) const override { return "VGM (libvgm)"; }
	juce::StringArray getFileExtensions (void) const override;

	bool canOpen (const playback::SongMemory& memory) const override;
	std::unique_ptr<playback::ISongInstance> open (const playback::SongMemory& memory,
												  juce::String& errorMessage) override;
};

} // namespace toybox::backends::libvgm
