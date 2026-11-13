#include "LibvgmVgz.h"

#include "stdtype.h"
#include "utils/DataLoader.h"
#include "utils/MemoryLoader.h"

namespace toybox::backends::libvgm
{

bool isCompressedImage (const void* data, std::size_t size)
{
	const auto* raw = static_cast<const unsigned char*> (data);
	return (raw != nullptr && size >= 2 && raw[0] == 0x1f && raw[1] == 0x8b);
}

bool flattenCompressedImage (const void* data, std::size_t size, juce::MemoryBlock& plain)
{
	if (data == nullptr || size == 0)
	{
		return false;
	}

	if (! isCompressedImage (data, size))
	{
		return true;	// nothing to do - the caller keeps its own image
	}

	DATA_LOADER* loader = MemoryLoader_Init ((const uint8_t*) data, (uint32_t) size);
	if (loader == NULL)
	{
		return false;
	}

	// **Bounded on purpose.** libvgm's loader inflates the whole stream into memory before its size
	// is known, so a small `.vgz` can ask for gigabytes. The cap is generous - the largest real VGM
	// image is megabytes, not tens of them - and refusing above it is better than letting the
	// allocation fail. The same discipline as `SessionState.cpp`'s own gzip read.
	constexpr uint32_t kMaxImageBytes = 64u * 1024 * 1024;

	bool success = false;
	if (DataLoader_Load (loader) == 0x00)
	{
		const uint32_t plainSize = DataLoader_GetSize (loader);
		const uint8_t* plainData = DataLoader_GetData (loader);
		if (plainData != nullptr && plainSize > 0 && plainSize <= kMaxImageBytes)
		{
			juce::MemoryBlock inflated (plainData, (size_t) plainSize);
			plain.swapWith (inflated);
			success = true;
		}
	}

	DataLoader_Deinit (loader);
	return success;
}

} // namespace toybox::backends::libvgm
