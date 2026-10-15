#include "BackendRegistry.h"

namespace vgmplay::playback
{

BackendRegistry& BackendRegistry::getInstance (void)
{
	static BackendRegistry instance;
	return instance;
}

void BackendRegistry::add (std::unique_ptr<ISongBackend> backend)
{
	if (backend != nullptr)
	{
		backends.push_back (std::move (backend));
	}
}

ISongBackend* BackendRegistry::findFor (const SongMemory& memory) const
{
	// the first backend that recognises the image wins - registration order is the priority
	for (const auto& backend : backends)
	{
		if (backend->canOpen (memory))
		{
			return backend.get();
		}
	}

	return nullptr;
}

juce::StringArray BackendRegistry::getFileExtensions (void) const
{
	juce::StringArray extensions;
	for (const auto& backend : backends)
	{
		extensions.addArray (backend->getFileExtensions());
	}

	extensions.removeDuplicates (false);
	return extensions;
}

juce::StringArray BackendRegistry::getBackendNames (void) const
{
	juce::StringArray names;
	for (const auto& backend : backends)
	{
		names.add (backend->getName());
	}

	return names;
}

} // namespace vgmplay::playback
