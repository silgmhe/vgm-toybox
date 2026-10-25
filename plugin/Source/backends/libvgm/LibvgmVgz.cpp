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

	bool success = false;
	if (DataLoader_Load (loader) == 0x00)
	{
		const uint32_t plainSize = DataLoader_GetSize (loader);
		const uint8_t* plainData = DataLoader_GetData (loader);
		if (plainData != nullptr && plainSize > 0)
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
