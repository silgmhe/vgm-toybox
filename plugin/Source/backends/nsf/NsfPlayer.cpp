#include "backends/nsf/NsfPlayer.h"

#include <algorithm>
#include <cmath>

#include "backends/nsf/NsfBus.h"

namespace toybox::backends::nsf
{

namespace
{

/** NTSC 2A03 cycle rate. A PAL machine runs at 1662607. Both are the CPU's own clock, not a
	multiple: the APU runs from the same crystal divided internally. */
constexpr double kNtscClock = 1789772.727272727;
constexpr double kPalClock = 1662607.0;

} // namespace

NsfPlayer::NsfPlayer (const formats::nsf::NsfFile& image, double sampleRate)
	: file (image), rate (sampleRate > 0.0 ? sampleRate : 44100.0)
{
	bus = std::make_unique<NsfBus> (file);
	bus->onSoundWrite = [this] (std::uint16_t address, std::uint8_t value)
	{
		writeSound (address, value);
	};

	cpu = std::make_unique<xgm::NES_CPU> (kNtscClock);
	apu = std::make_unique<xgm::NES_APU> ();
	dmc = std::make_unique<xgm::NES_DMC> ();

	const double clock = file.isPal() ? kPalClock : kNtscClock;

	apu->SetClock (clock);
	apu->SetRate (rate);
	dmc->SetClock (clock);
	dmc->SetRate (rate);
	dmc->SetPal (file.isPal ());

	// The DMC fetches its sample bytes through a memory device; without one the first DPCM read
	// dereferences a null pointer, and a driver is allowed to start one during init.
	dmc->SetMemory (bus.get ());
	dmc->SetAPU (apu.get ());
	dmc->SetCPU (cpu.get ());
}

NsfPlayer::~NsfPlayer () = default;

void NsfPlayer::start (void)
{
	rendered = 0;
	cpuClockRest = 0.0;

	// Order matters, and each step was paid for once already:
	//   1. the image, then the trampoline (SetImage clears the 64 KB it lives in);
	//   2. the bus before the CPU, because Reset() installs the callbacks that read through it;
	//   3. Start() last, which is what actually runs the driver's init.
	bus->load ();

	// NSFPlay resets every attached device through its stack before starting; we own the two
	// devices directly, so they are reset here. Without this the frame sequencer has no phase and
	// TickFrameSequence() has nothing to advance against.
	apu->Reset ();
	dmc->Reset ();

	cpu->SetMemory (bus.get ());
	cpu->SetNESMemory (bus.get ());
	cpu->nes_basecycles = file.isPal () ? kPalClock : kNtscClock;
	cpu->Reset ();

	const int song = file.startSong > 0 ? file.startSong - 1 : 0;
	const int region = file.isPal () ? 1 : 0;

	applyMasks ();

	cpu->Start ((int) file.initAddress, (int) file.playAddress, file.framesPerSecond (), song,
				region, 0, false, nullptr);

}

void NsfPlayer::writeSound (std::uint16_t address, std::uint8_t value)
{
	// $4000-$4013 are the pulse, triangle and noise registers, $4015 the status/enable mask.
	if (address <= 0x4013 || address == 0x4015)
	{
		apu->Write (address, value);
		return;
	}

	if (address == 0x4011)
	{
		dmc->Write (address, value);
		return;
	}

	if (address == 0x4017)
	{
		// The frame counter register is shared: the APU sequences on it and the DMC's IRQ does too.
		apu->Write (address, value);
		dmc->Write (address, value);
	}
}

int NsfPlayer::render (float* left, float* right, int frames)
{
	if (frames <= 0)
	{
		return 0;
	}


	const double clocksPerSample = (file.isPal () ? kPalClock : kNtscClock) / rate;

	for (int i = 0; i < frames; ++i)
	{
		cpuClockRest += clocksPerSample;


		const auto clocks = (int) cpuClockRest;

		if (clocks > 0)
		{
			const int executed = cpu->Exec (clocks);
			cpuClockRest -= (double) executed;

			// Envelopes and length counters live here; Tick() alone leaves every note at the level
			// it was given.
			dmc->TickFrameSequence ((UINT32) (std::max) (0, executed));

			apu->Tick ((UINT32) (std::max) (0, executed));
			dmc->Tick ((UINT32) (std::max) (0, executed));
		}

		INT32 a[2] = { 0, 0 };
		INT32 d[2] = { 0, 0 };
		apu->Render (a);
		dmc->Render (d);

		const auto mix = [&] (int channel, float& out)
		{
			const int value = (int) a[channel] + (int) d[channel];
			out = (std::clamp) ((float) value / 32768.0f, -1.0f, 1.0f);
		};

		mix (0, left[i]);
		mix (1, right[i]);
	}

	rendered += (std::uint64_t) frames;
	return frames;
}

std::uint64_t NsfPlayer::soundWrites (void) const
{
	return bus != nullptr ? bus->soundWriteCount : 0;
}

double NsfPlayer::positionSeconds (void) const
{
	return (double) rendered / rate;
}

void NsfPlayer::setTrackAudible (int trackIndex)
{
	soloTrack = trackIndex;
	applyMasks ();
}

void NsfPlayer::applyMasks (void)
{
	// The chips mute with a set bit, and they are split across two objects: the APU renders the two
	// pulse channels, the DMC object renders triangle, noise and DPCM. A track index is mapped onto
	// those two masks here, which is the whole of the isolation.
	const bool all = soloTrack < 0;

	const bool pulse1 = all || soloTrack == 0;
	const bool pulse2 = all || soloTrack == 1;
	const bool triangle = all || soloTrack == 2;
	const bool noise = all || soloTrack == 3;
	const bool dmcChannel = all || soloTrack == 4;

	int apuMask = 0;
	if (! pulse1) { apuMask |= 1; }
	if (! pulse2) { apuMask |= 2; }

	int dmcMask = 0;
	if (! triangle) { dmcMask |= 1; }
	if (! noise) { dmcMask |= 2; }
	if (! dmcChannel) { dmcMask |= 4; }

	apu->SetMask (apuMask);
	dmc->SetMask (dmcMask);
}

} // namespace toybox::backends::nsf
