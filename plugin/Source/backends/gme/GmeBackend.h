#pragma once

#include <memory>

#include "playback/SongBackend.h"

/**
	The NES formats, rendered by libgme.

	NSF is not a register dump: it is a 6502 program that drives the sound chips, so playing one
	means running it. libgme already does exactly that - its `Nsf_Emu` owns the CPU, the APU, the
	DPCM and the expansion chips, and handles bank switching - which is why this backend is a thin
	adapter rather than an emulator of our own.

	Isolation: libgme can point each 2A03 oscillator at its own output, and NULL mutes
	one, so the backend offers a track per oscillator and reports multiInstance. Muting
	is one-way through that call, so a player is built for exactly one track and never
	restored - the core creates one player per track, five emulations in all.
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
