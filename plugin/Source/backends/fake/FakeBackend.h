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

/**
	Test hook: how long `open()` pretends to take, in milliseconds (0 = instant, the default).

	The core test has to answer "is the audio thread starved while a song loads", and the fake
	backend loads instantly - so there would be nothing to measure. A delay stands in for what a real
	backend does there: unpacking a .vgz and starting the sound cores.
*/
void setLoadDelayMilliseconds (int milliseconds);
int getLoadDelayMilliseconds (void);

} // namespace toybox::backends::fake
