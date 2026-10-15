#pragma once

#include <memory>
#include <vector>

#include "SongBackend.h"

namespace vgmplay::playback
{

/**
	The list of format backends this build knows about.

	Backends are registered at compile time (the product is a VST3 plugin, so everything ships
	inside one signed binary - there is deliberately no runtime loading, see ADR-008 in
	docs/playback-backend-design.md). Optional backends register under a compile-time switch,
	so a build without them simply has one fewer entry here.

	The registry is filled once (from the plugin constructor) and read by the loader and by the
	UI (file dialog filter, drop hint) - all on the message thread.
*/
class BackendRegistry
{
public:
	static BackendRegistry& getInstance (void);

	/** Registers a backend; takes ownership. Order matters: the first match wins. */
	void add (std::unique_ptr<ISongBackend> backend);

	/** The backend that can open this image, or nullptr when no registered backend can. */
	ISongBackend* findFor (const SongMemory& memory) const;

	/** All extensions of all backends, for the file chooser ("*.vgm;*.vgz;..."). */
	juce::StringArray getFileExtensions (void) const;

	/** Human-readable list of the registered backends ("VGM (libvgm)", ...) for the tooltips. */
	juce::StringArray getBackendNames (void) const;

	int getNumBackends (void) const { return (int) backends.size(); }

private:
	BackendRegistry (void) = default;

	std::vector<std::unique_ptr<ISongBackend>> backends;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BackendRegistry)
};

/** Registers the backends this build was compiled with - call once, from the plugin. */
void registerBuiltInBackends (void);

} // namespace vgmplay::playback
