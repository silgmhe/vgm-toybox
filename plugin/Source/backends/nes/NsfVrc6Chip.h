#pragma once

// The VRC6's three extra voices, rendered by NSFPlay's core.
//
// This is the chip the VGM route could not reach at all: the format has no command for VRC6, so an
// FTM that uses it could only ever play its 2A03 half. The core takes the same thing every other
// NES chip does - `Write(address, value)` - so the module compiler's output drives it unchanged.

#include <cstdint>
#include <memory>

namespace xgm
{
class NES_VRC6;
}

namespace toybox::backends::nes
{

class NsfVrc6Chip
{
public:
	explicit NsfVrc6Chip (double sampleRate);
	~NsfVrc6Chip();

	NsfVrc6Chip (const NsfVrc6Chip&) = delete;
	NsfVrc6Chip& operator= (const NsfVrc6Chip&) = delete;

	/** A register of the chip's own map: $9000-$9002, $A000-$A002, $B000-$B002. */
	void write (int address, int value);

	/** Muted voices, one bit each, in the tracker's order: pulse 1, pulse 2, sawtooth. */
	void setMask (int mask);

	/** Adds `samples` frames into the two buffers, like the APU wrapper. */
	void render (float* left, float* right, int samples);

	static constexpr float fullScale = 32768.0f;

private:
	std::unique_ptr<xgm::NES_VRC6> chip;
	double clockHz = 1789773.0;
	double clocksPerSample = 1.0;
	double clockAccumulator = 0.0;
};

} // namespace toybox::backends::nes
