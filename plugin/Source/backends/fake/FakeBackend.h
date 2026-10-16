#pragma once

#include "playback/SongBackend.h"

namespace toybox::backends::fake
{

/**
	A backend with no format engine behind it at all: it "plays" three synthesised tracks (three
	sine tones), one per channel of two fake devices.

	Its job is to make the decoupling *provable*: the core test target (vgm_core_test) links the
	playback core plus this backend and **no libvgm code whatsoever**, so a core that still
	depended on libvgm would not link. It also gives the mixer parameters a back end whose output
	is analytically known (a sine of amplitude 0.25), which is far tighter than comparing two
	emulations.

	A file is recognised by its ".fake" extension; the contents are not read.
*/
class FakeBackend final : public playback::ISongBackend
{
public:
	juce::String getName (void) const override { return "Fake tones (test backend)"; }
	juce::StringArray getFileExtensions (void) const override { return { ".fake" }; }

	bool canOpen (const playback::SongMemory& memory) const override;
	std::unique_ptr<playback::ISongInstance> open (const playback::SongMemory& memory,
												  juce::String& errorMessage) override;
};

} // namespace toybox::backends::fake
