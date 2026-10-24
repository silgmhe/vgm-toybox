#pragma once

#include "playback/SongBackend.h"

/**
	The NES formats, played by NSFPlay's cores.

	An NSF is a 6502 program that writes sound registers, not a recording of them, so playing one
	means running it. The cores that do the running are vendored in libs/nsfplay; what this backend
	adds is the reading of the header, the address space the driver runs in, and the mapping onto
	the three ports the rest of the program is written against.
*/
namespace toybox::backends::nsf
{

class NsfBackend final : public playback::ISongBackend
{
public:
	juce::String getName (void) const override { return "NSF (NSFPlay core)"; }
	juce::StringArray getFileExtensions (void) const override { return { ".nsf" }; }

	bool canOpen (const playback::SongMemory& memory) const override;
	std::unique_ptr<playback::ISongInstance> open (const playback::SongMemory& memory,
												   juce::String& errorMessage) override;
};

} // namespace toybox::backends::nsf
