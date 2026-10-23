#include "LibvgmBackend.h"

#include "LibvgmSongInstance.h"
#include "LibvgmVgz.h"

namespace toybox::backends::libvgm
{

juce::StringArray LibvgmBackend::getFileExtensions (void) const
{
	// .ftm is listed because it plays: the module is compiled into a VGM image on open (see
	// LibvgmSongInstance::unpackImage), so the backend can hand it to the same sound cores.
	return { ".vgm", ".vgz", ".s98", ".dro", ".gym", ".ftm" };
}

bool LibvgmBackend::canOpen (const playback::SongMemory& memory) const
{
	const auto extension = memory.file.getFileExtension().toLowerCase();
	if (getFileExtensions().contains (extension, true))
	{
		return true;
	}

	// a .vgz that lost its file name is still recognisable through the gzip magic, and so is a
	// module - both are decided by the bytes rather than by whatever the file is called
	return isCompressedImage (memory.data, memory.size) || isFtmModule (memory.data, memory.size);
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
