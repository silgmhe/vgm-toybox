#pragma once

// The N163's wave-table voices, rendered by NSFPlay's core.
//
// The chip is addressed through a select port: the register number goes to $F800 (bit 7 asks for
// auto-increment) and the value to $4800. Its eight voices are not independent - one is clocked at
// a time - which is why the tracker has to tell the core how many are in use.

#include <cstdint>
#include <memory>

namespace xgm
{
class NES_N106;
}

namespace toybox::backends::nes
{

class NsfN163Chip
{
public:
	explicit NsfN163Chip (double sampleRate);
	~NsfN163Chip();

	NsfN163Chip (const NsfN163Chip&) = delete;
	NsfN163Chip& operator= (const NsfN163Chip&) = delete;

	/** A register access: $F800 selects, $4800 writes the selected register. */
	void write (int address, int value);

	void render (float* left, float* right, int samples);

	static constexpr float fullScale = 32768.0f;

private:
	std::unique_ptr<xgm::NES_N106> chip;
	double clockHz = 1789773.0;
	double clocksPerSample = 1.0;
	double clockAccumulator = 0.0;
};

} // namespace toybox::backends::nes
