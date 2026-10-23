#pragma once

// The VRC7's six FM voices, rendered by NSFPlay's OPLL core.
//
// The chip is an OPLL (YM2413 family) with sixteen instruments in ROM and a register set addressed
// through two ports: 0x9010 takes the register number, 0x9030 the value. FamiTracker drives it as
// six channels of one operator pair each, so a note is three writes - frequency low, patch and
// volume, then frequency high with the block and the key bits.

#include <cstdint>
#include <memory>

namespace xgm
{
class NES_VRC7;
}

namespace toybox::backends::nes
{

class NsfVrc7Chip
{
public:
	explicit NsfVrc7Chip (double sampleRate);
	~NsfVrc7Chip();

	NsfVrc7Chip (const NsfVrc7Chip&) = delete;
	NsfVrc7Chip& operator= (const NsfVrc7Chip&) = delete;

	/** A register of the chip's map, $9010-$9030. */
	void write (int address, int value);

	/** Muted voices, one bit each, in the tracker's order. */
	void setMask (int mask);

	void render (float* left, float* right, int samples);

	static constexpr float fullScale = 32768.0f;

private:
	std::unique_ptr<xgm::NES_VRC7> chip;
	double clockHz = 1789773.0;
	double clocksPerSample = 1.0;
	double clockAccumulator = 0.0;
};

} // namespace toybox::backends::nes
