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
	// Only the expansion the header declares exists, and each one is fed by the writes its own
	// address range receives - the routing below is the whole of the contract with them.
	const auto chip = file.chip ();

	if (chip == formats::nsf::NsfChip::vrc6)
	{
		vrc6 = std::make_unique<xgm::NES_VRC6> ();
		vrc6->SetClock (clock);
		vrc6->SetRate (rate);
	}

	if (chip == formats::nsf::NsfChip::vrc7)
	{
		vrc7 = std::make_unique<xgm::NES_VRC7> ();
		vrc7->SetClock (clock);
		vrc7->SetRate (rate);
	}

	if (chip == formats::nsf::NsfChip::n163)
	{
		n106 = std::make_unique<xgm::NES_N106> ();
		n106->SetClock (clock);
		n106->SetRate (rate);
	}

	if (chip == formats::nsf::NsfChip::fme7)
	{
		fme7 = std::make_unique<xgm::NES_FME7> ();
		fme7->SetClock (clock);
		fme7->SetRate (rate);
	}

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

	if (vrc6) { vrc6->Reset (); }
	if (vrc7) { vrc7->Reset (); }
	if (n106) { n106->Reset (); }
	if (fme7) { fme7->Reset (); }

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

	if (vrc6 && address >= 0x9000 && address <= 0xB003)
	{
		vrc6->Write (address, value);
		return;
	}

	if (vrc7 && (address == 0x9010 || address == 0x9030))
	{
		vrc7->Write (address, value);
		return;
	}

	if (n106 && (address == 0x4800 || address == 0xF800))
	{
		n106->Write (address, value);
		return;
	}

	if (fme7 && (address == 0xC000 || address == 0xE000))
	{
		fme7->Write (address, value);
		return;
	}

	if (address == 0x4017)
	{
		// The frame counter register is shared: the APU sequences on it and the DMC's IRQ does too.
		apu->Write (address, value);
		dmc->Write (address, value);
	}
}

bool NsfPlayer::hasExpansion (void) const
{
	return vrc6 != nullptr || vrc7 != nullptr || n106 != nullptr || fme7 != nullptr;
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

			const auto clocks = (UINT32) (std::max) (0, executed);
			apu->Tick (clocks);
			dmc->Tick (clocks);

			if (vrc6) { vrc6->Tick (clocks); }
			if (vrc7) { vrc7->Tick (clocks); }
			if (n106) { n106->Tick (clocks); }
			if (fme7) { fme7->Tick (clocks); }
		}

		INT32 a[2] = { 0, 0 };
		INT32 d[2] = { 0, 0 };
		apu->Render (a);
		dmc->Render (d);

		INT32 extra[2] = { 0, 0 };

		const auto addChip = [&extra] (xgm::ISoundChip* chip)
		{
			if (chip == nullptr)
			{
				return;
			}

			INT32 out[2] = { 0, 0 };
			chip->Render (out);
			extra[0] += out[0];
			extra[1] += out[1];
		};

		addChip (vrc6.get ());
		addChip (vrc7.get ());
		addChip (n106.get ());
		addChip (fme7.get ());

		const auto mix = [&] (int channel, float& out)
		{
			const int value = (int) a[channel] + (int) d[channel] + (int) extra[channel];
			out = (std::clamp) ((float) value / 32768.0f, -1.0f, 1.0f);
		};

		mix (0, left[i]);
		mix (1, right[i]);
	}

	rendered += (std::uint64_t) frames;
	return frames;
}

std::uint32_t NsfPlayer::expansionWrites (int index) const
{
	if (bus == nullptr || index < 0 || index >= 4)
	{
		return 0;
	}

	return bus->expansionWrites[(std::size_t) index];
}

std::uint64_t NsfPlayer::soundWrites (void) const
{
	return bus != nullptr ? bus->soundWriteCount : 0;
}

double NsfPlayer::positionSeconds (void) const
{
	return (double) rendered / rate;
}

bool NsfPlayer::seekTo (double seconds)
{
	if (seconds < 0.0)
	{
		return false;
	}

	// Restart, then run the driver until the position is reached. There is no state to seek into:
	// the music lives in a program that has to be executed from its beginning.
	start ();

	// Big steps, not one sample at a time. The CPU keeps its own frame schedule, so the size of the
	// step does not change when play is called - it only decides how often this loop looks at the
	// clock. Forty clocks per step would be one sample's worth; four thousand is a hundred times
	// less bookkeeping for exactly the same emulation.
	constexpr int clocksPerStep = 4096;
	const auto target = (std::uint64_t) (seconds * rate);

	while (rendered < target)
	{
		const int executed = cpu->Exec (clocksPerStep);
		const auto clocks = (UINT32) (std::max) (0, executed);

		dmc->TickFrameSequence (clocks);
		apu->Tick (clocks);
		dmc->Tick (clocks);

		if (vrc6) { vrc6->Tick (clocks); }
		if (vrc7) { vrc7->Tick (clocks); }
		if (n106) { n106->Tick (clocks); }
		if (fme7) { fme7->Tick (clocks); }

		rendered += (std::uint64_t) executed;
	}

	return true;
}

void NsfPlayer::setTrackAudible (int trackIndex)
{
	soloTrack = trackIndex;
	applyMasks ();
}

int NsfPlayer::channelCount (void) const
{
	// Five on the 2A03 - the two the APU renders and the three the DMC object renders - plus the
	// expansion chip's own, of which a file can declare only one.
	if (vrc6) { return 5 + 3; }
	if (n106) { return 5 + 8; }
	if (vrc7) { return 5 + 6; }
	if (fme7) { return 5 + 5; }

	return 5;
}

void NsfPlayer::applyMasks (void)
{
	// Each chip mutes with a set bit over its own outputs, and the track list is the concatenation
	// of those outputs in this order: two from the APU, three from the DMC object, then the
	// expansion's. A solo is therefore one bit cleared in exactly one of these masks.
	const bool all = soloTrack < 0;

	const auto maskFor = [all, this] (int base, int count)
	{
		int mask = 0;

		for (int i = 0; i < count; ++i)
		{
			if (! all && soloTrack != base + i)
			{
				mask |= (1 << i);
			}
		}

		return mask;
	};

	apu->SetMask (maskFor (0, 2));
	dmc->SetMask (maskFor (2, 3));

	if (vrc6) { vrc6->SetMask (maskFor (5, 3)); }
	if (n106) { n106->SetMask (maskFor (5, 8)); }
	if (vrc7) { vrc7->SetMask (maskFor (5, 6)); }
	if (fme7) { fme7->SetMask (maskFor (5, 5)); }
}

int NsfPlayer::renderChannels (float* const* dest, int channelCount, int frames)
{
	if (frames <= 0 || dest == nullptr)
	{
		return 0;
	}

	const double clocksPerSample = (file.isPal () ? kPalClock : kNtscClock) / rate;
	const int usable = (std::min) (channelCount, 5);	// the two 2A03 objects, for now

	for (int i = 0; i < frames; ++i)
	{
		cpuClockRest += clocksPerSample;

		const auto clocks = (int) cpuClockRest;

		if (clocks > 0)
		{
			const int executed = cpu->Exec (clocks);
			cpuClockRest -= (double) executed;

			const auto tick = (UINT32) (std::max) (0, executed);
			dmc->TickFrameSequence (tick);
			apu->Tick (tick);
			dmc->Tick (tick);

			if (vrc6) { vrc6->Tick (tick); }
			if (vrc7) { vrc7->Tick (tick); }
			if (n106) { n106->Tick (tick); }
			if (fme7) { fme7->Tick (tick); }
		}

		INT32 squares[4] = { 0, 0, 0, 0 };
		INT32 tnd[6] = { 0, 0, 0, 0, 0, 0 };

		apu->RenderChannels (squares, 2, 2);
		dmc->RenderChannels (tnd, 2, 3);

		for (int c = 0; c < 2 && c < usable; ++c)
		{
			dest[c][2 * i + 0] = (float) squares[c * 2 + 0] / 32768.0f;
			dest[c][2 * i + 1] = (float) squares[c * 2 + 1] / 32768.0f;
		}

		for (int c = 0; c < 3 && 2 + c < usable; ++c)
		{
			dest[2 + c][2 * i + 0] = (float) tnd[c * 2 + 0] / 32768.0f;
			dest[2 + c][2 * i + 1] = (float) tnd[c * 2 + 1] / 32768.0f;
		}

		// The expansion chip's channels are not filled yet: it has no RenderChannels of its own,
		// and pretending otherwise would give tracks that are silent for a reason nobody can see.
		for (int c = 5; c < channelCount; ++c)
		{
			dest[c][2 * i + 0] = 0.0f;
			dest[c][2 * i + 1] = 0.0f;
		}
	}

	rendered += (std::uint64_t) frames;
	return frames;
}

} // namespace toybox::backends::nsf
