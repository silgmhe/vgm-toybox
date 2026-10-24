#pragma once

#include <cstdint>
#include <memory>

#include "formats/nsf/NsfFile.h"
#include "xgm/devices/CPU/nes_cpu.h"
#include "xgm/devices/Sound/nes_apu.h"
#include "xgm/devices/Sound/nes_dmc.h"

namespace toybox::backends::nsf
{

class NsfBus;

/**
	One NSF, running.

	The division of labour is the one NSFPlay settled on, and it is worth stating because it is not
	obvious: the CPU core owns the frame schedule. Exec() runs until the driver's init or play
	returns (it breaks on the JMP-to-self at $4103) and then, when its own frame counter expires,
	re-enters play by itself. A player therefore only has to hand it clocks and tick the chips - it
	must not count frames, or it will call play twice per frame and play nothing at all.

	The DMC's frame sequencer is separate again: Tick() only computes triangle, noise and DPCM, so
	envelopes and length counters advance only when TickFrameSequence() is called alongside it.
*/
class NsfPlayer
{
public:
	NsfPlayer (const formats::nsf::NsfFile& file, double sampleRate);
	~NsfPlayer ();

	NsfPlayer (const NsfPlayer&) = delete;
	NsfPlayer& operator= (const NsfPlayer&) = delete;

	/** Builds the address space, resets the CPU and runs the driver's init. Message thread. */
	void start (void);

	/** Renders `frames` stereo frames. Returns how many were written. */
	int render (float* left, float* right, int frames);

	/** Solos one of the five channels; -1 leaves them all audible. */
	void setTrackAudible (int trackIndex);

	std::uint64_t soundWrites (void) const;
	double positionSeconds (void) const;

private:
	void writeSound (std::uint16_t address, std::uint8_t value);
	void applyMasks (void);
	int soloTrack = -1;

	const formats::nsf::NsfFile& file;
	double rate = 44100.0;
	double cpuClockRest = 0.0;
	std::uint64_t rendered = 0;

	std::unique_ptr<NsfBus> bus;
	std::unique_ptr<xgm::NES_CPU> cpu;
	std::unique_ptr<xgm::NES_APU> apu;
	std::unique_ptr<xgm::NES_DMC> dmc;
};

} // namespace toybox::backends::nsf
