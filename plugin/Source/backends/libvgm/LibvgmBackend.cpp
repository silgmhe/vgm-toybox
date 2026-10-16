#include "LibvgmBackend.h"

#include "LibvgmSongInstance.h"
#include "LibvgmVgz.h"

namespace toybox::backends::libvgm
{

juce::StringArray LibvgmBackend::getFileExtensions (void) const
{
	return { ".vgm", ".vgz", ".s98", ".dro", ".gym" };
}

bool LibvgmBackend::canOpen (const playback::SongMemory& memory) const
{
	const auto extension = memory.file.getFileExtension().toLowerCase();
	if (getFileExtensions().contains (extension, true))
	{
		return true;
	}

	// a .vgz that lost its file name is still recognisable through the gzip magic
	return isCompressedImage (memory.data, memory.size);
}

std::unique_ptr<playback::ISongInstance> LibvgmBackend::open (const playback::SongMemory& memory,
															  juce::String& errorMessage)
{
	auto instance = std::make_unique<LibvgmSongInstance> (memory);
	if (! instance->unpackImage (errorMessage))
	{
		return nullptr;
	}

	return instance;
}

} // namespace toybox::backends::libvgm
