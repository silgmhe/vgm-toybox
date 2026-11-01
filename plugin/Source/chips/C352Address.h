#pragma once

#include <cstddef>
#include <cstdint>

namespace toybox::chips
{
	/** How many bytes a C352 voice's `start..end` covers, in the chip's wrapping window.

		Not `end - start + 1`. The core keeps a position that only ever moves forward and compares
		its low sixteen bits against `end` (`c352.c`, `C352_fetch_sample`), so an endpoint below the
		start means the sample runs off the top of its sixty-four-kilobyte window and continues at
		the bottom. Subtracting anyway underflows to nearly sixty-five thousand, which is how a row
		ended up drawing the wrong bytes - or, where the caller guarded on `end >= start`, drawing
		nothing while the channel played.

		One copy for the two readers that need it: the decoder that draws the wave and the collector
		that exports it. Two copies of this arithmetic is the shape of the bug that once made an
		exported drum and a drawn one disagree.
	*/
	inline std::size_t c352SampleLength (std::uint32_t start, std::uint32_t end)
	{
		return (end >= start) ? (std::size_t) (end - start) + 1
							  : (std::size_t) (0x10000u - start + end) + 1;
	}
}
