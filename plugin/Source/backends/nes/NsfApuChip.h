#pragma once

// The 2A03, rendered by NSFPlay's cores: two squares in NES_APU, and the triangle, noise and DPCM
// in NES_DMC (the core is split the way the chip is - the DMC block owns the frame sequencer, which
// is why it drives the APU's frame sequence).
//
// Driving them from register writes needs a little more than the squares did: the DMC reads sample
// bytes through a memory device at $8000, and it announces cycle steals to a CPU that is not
// running here. Both are supplied - an inert CPU object and a read-only view of the sample ROM -
// so the core's own code path is the one that runs, rather than a stubbed one.

#include <cstddef>
#include <cstdint>
#include <memory>

namespace xgm
{
class NES_APU;
class NES_DMC;
class NES_CPU;
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

	/** A register of the 2A03's map, $4000-$4017. Writes are routed the way the core splits them:
		the squares to one device, the triangle, noise and DPCM to the other, and $4015 to both. */
	void write (int address, int value);

	/** The DPCM sample area. The chip reads it at $8000, which is where the caller's image starts
		according to the format, so the copy is only needed when the bytes are not already there. */
	void setDpcmRom (const std::uint8_t* bytes, std::size_t length);

	/** Muted channels, one bit each: squares in the low two bits, triangle, noise and DPCM next. */
	void setMask (int squareMask, int tndMask);

	void render (float* left, float* right, int samples);

	static constexpr float fullScale = 32768.0f;

private:
	struct Rom;

	std::unique_ptr<xgm::NES_APU> apu;
	std::unique_ptr<xgm::NES_DMC> dmc;
	std::unique_ptr<xgm::NES_CPU> cpu;
	std::unique_ptr<Rom> rom;

	double clockHz = 1789773.0;
	double clocksPerSample = 1.0;
	double clockAccumulator = 0.0;
};

} // namespace toybox::backends::nes
