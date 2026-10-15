#pragma once

#include <juce_core/juce_core.h>

namespace vgmplay::backends::libvgm
{

/** True when the image starts with the gzip magic - a .vgz wrapped VGM. */
bool isCompressedImage (const void* data, std::size_t size);

/** Inflates a .vgz image into `plain`, so that every track's loader works on the same plain
	bytes instead of holding a decompressed copy of its own - a mixer track list easily reaches
	a dozen players. Plain images pass through unchanged. */
bool flattenCompressedImage (const void* data, std::size_t size, juce::MemoryBlock& plain);

} // namespace vgmplay::backends::libvgm
