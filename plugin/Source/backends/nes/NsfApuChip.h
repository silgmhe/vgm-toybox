#pragma once

// One 2A03, rendered by NSFPlay's core.
//
// The reason this exists rather than extending libvgm: libvgm ships three NES-family cores (APU,
// DMC, FDS) and the VGM format has no command for VRC6, MMC5, N163 or S5B, so the expansion chips
// cannot reach a speaker through that path at all. NSFPlay has all eight behind one interface whose
// write is `(address, value)` - the shape the module compiler already emits.
//
// The wrapper is deliberately thin: clocks, a write, and a render that converts to float. Anything
// cleverer would be a second mixer.

#include <cstdint>
#include <memory>

namespace xgm
{
class NES_APU;
}

namespace toybox::backends::nes
{

class NsfApuChip
{
public:
	explicit NsfApuChip (double sampleRate);
	~NsfApuChip();

	NsfApuChip (const NsfApuChip&) = delete;
	NsfApuChip& operator= (const NsfApuChip&) = delete;

	/** A register of the 2A03's own map, $4000-$4017. */
	void write (int address, int value);

	/** Adds `samples` frames into the two buffers. Additive so several chips can share a bus. */
	void render (float* left, float* right, int samples);

	/** The core's output swing, so a caller can normalise several chips against each other. */
	static constexpr float fullScale = 32768.0f;

private:
	std::unique_ptr<xgm::NES_APU> apu;
	double clockHz = 1789773.0;
	double clocksPerSample = 1.0;
	double clockAccumulator = 0.0;
};

} // namespace toybox::backends::nes
