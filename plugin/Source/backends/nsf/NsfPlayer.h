#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "formats/nsf/NsfFile.h"

namespace xgm
{
class NES_CPU;
class NES_APU;
class NES_DMC;
class NES_VRC6;
class NES_VRC7;
class NES_N106;
}

namespace toybox::backends::nsf
{

/**
	An NSF, played: the driver is a 6502 program, so playing one means running it and letting the
	chips answer the writes it makes.

	The pieces are deliberately separate. `NsfFile` reads the header, `NsfMemory` is the address
	space, this class owns the CPU and the chips and decides when each runs. The CPU core is told
	the init and play addresses and the frame rate once; from then on it calls them itself, which is
	why there is no per-frame bookkeeping here beyond ticking the chips for the same number of
	clocks the CPU just ran.
*/
class NsfPlayer
{
public:
	explicit NsfPlayer (double sampleRate);
	~NsfPlayer();

	NsfPlayer (const NsfPlayer&) = delete;
	NsfPlayer& operator= (const NsfPlayer&) = delete;

	/** Reads the image, maps it, and starts the driver. False with a reason for a bad file. */
	bool load (const void* data, std::size_t size, std::string& error);

	/** Fills the two buffers with `samples` frames. Adds, like the chip wrappers do. */
	void render (float* left, float* right, int samples);

	double positionSeconds (void) const;
	bool isFinished (void) const { return finished; }

	/** Which expansion the file asked for, and whether the header says PAL. */
	toybox::formats::nsf::NsfChip chip (void) const { return file.chip; }
	bool isPal (void) const { return pal; }

	static constexpr float fullScale = 32768.0f;

private:
	class Bus;

	friend class Bus;

	bool writeExpansion (std::uint16_t address, std::uint8_t value);
	void writeSoundRegister (std::uint16_t address, std::uint8_t value);
	void tickChips (std::uint32_t clocks);
	void renderChips (float* left, float* right);

	toybox::formats::nsf::NsfFile file;
	std::unique_ptr<Bus> bus;
	std::unique_ptr<xgm::NES_CPU> cpu;
	std::unique_ptr<xgm::NES_APU> apu;
	std::unique_ptr<xgm::NES_DMC> dmc;
	std::unique_ptr<xgm::NES_VRC6> vrc6;
	std::unique_ptr<xgm::NES_VRC7> vrc7;
	std::unique_ptr<xgm::NES_N106> n163;

	double rate = 44100.0;
	double clockHz = 1789773.0;		// NTSC; PAL sets its own below
	double clocksPerSample = 40.58;
	double accumulator = 0.0;
	double seconds = 0.0;
	bool pal = false;
	bool running = false;
	bool finished = false;
};

} // namespace toybox::backends::nsf
