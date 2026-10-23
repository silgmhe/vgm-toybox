#include "LibvgmVgz.h"

#include <cstring>

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

bool isFtmModule (const void* data, std::size_t size)
{
	static const char kFamiTracker[] = "FamiTracker Module";
	static const char kDnFamiTracker[] = "Dn-FamiTracker Module";

	const auto* raw = static_cast<const char*> (data);
	if (raw == nullptr)
	{
		return false;
	}

	if (size >= sizeof (kFamiTracker) - 1
		&& std::memcmp (raw, kFamiTracker, sizeof (kFamiTracker) - 1) == 0)
	{
		return true;
	}

	return size >= sizeof (kDnFamiTracker) - 1
		   && std::memcmp (raw, kDnFamiTracker, sizeof (kDnFamiTracker) - 1) == 0;
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

	DATA_LOADER* loader = MemoryLoader_Init ((const UINT8*) data, (UINT32) size);
	if (loader == NULL)
	{
		return false;
	}

	bool success = false;
	if (DataLoader_Load (loader) == 0x00)
	{
		const UINT32 plainSize = DataLoader_GetSize (loader);
		const UINT8* plainData = DataLoader_GetData (loader);
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
