#pragma once

#include <memory>

#include "playback/SongBackend.h"

/**
	The NES formats, rendered by libgme.

	NSF is not a register dump: it is a 6502 program that drives the sound chips, so playing one
	means running it. libgme already does exactly that - its `Nsf_Emu` owns the CPU, the APU, the
	DPCM and the expansion chips, and handles bank switching - which is why this backend is a thin
	adapter rather than an emulator of our own.

	What that costs us: libgme renders the song as a whole, with no way to ask for one channel, so
	this backend reports `IsolationMode::none` and offers a single track. The mixer degrades to a
	mono strip rather than pretending it can isolate.
*/
namespace toybox::backends::gme
{

class GmeBackend final : public playback::ISongBackend
{
public:
	juce::String getName (void) const override { return "libgme"; }
	juce::StringArray getFileExtensions (void) const override { return { ".nsf", ".nsfe" }; }

	/** NSF images start with "NESM", and NSFe with "NSFE". */
	bool canOpen (const playback::SongMemory& memory) const override;

	std::unique_ptr<playback::ISongInstance> open (const playback::SongMemory& memory,
												   juce::String& errorMessage) override;
};

} // namespace toybox::backends::gme
