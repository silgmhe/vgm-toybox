#include "NsfApuChip.h"

#include <vector>

#include "third_party/nsfplay/xgm/devices/CPU/nes_cpu.h"
#include "third_party/nsfplay/xgm/devices/Sound/nes_apu.h"
#include "third_party/nsfplay/xgm/devices/Sound/nes_dmc.h"

namespace toybox::backends::nes
{

/** The DPCM sample area as the DMC sees it: bytes addressed from $8000. The core asks for them by
	address while it plays a sample; everything outside the range reads as zero. */
struct NsfApuChip::Rom final : xgm::IDevice
{
	std::vector<std::uint8_t> bytes;

	bool Read (UINT32 address, UINT32& value, UINT32) override
	{
		value = 0;

		if (address >= 0x8000)
		{
			const std::size_t offset = address - 0x8000;
			if (offset < bytes.size())
			{
				value = bytes[offset];
			}
		}

		return true;
	}

	// IDevice is the port the DMC reads its sample bytes through, and it asks for two things a
	// read-only view cannot do: a reset, which has nothing to clear, and a write, which has nowhere
	// to go. Both answer without touching the data.
	void Reset () override {}
	bool Write (UINT32, UINT32, UINT32 = 0) override { return false; }
};

NsfApuChip::NsfApuChip (double sampleRate)
	: apu (new xgm::NES_APU()),
	  dmc (new xgm::NES_DMC()),
	  cpu (new xgm::NES_CPU()),
	  rom (new Rom())
{
	const double rate = sampleRate > 0.0 ? sampleRate : 44100.0;

	apu->SetClock (clockHz);
	apu->SetRate (rate);
	apu->Reset();

	dmc->SetClock (clockHz);
	dmc->SetRate (rate);
	dmc->SetAPU (apu.get());
	dmc->SetCPU (cpu.get());
	dmc->SetMemory (rom.get());
	dmc->Reset();

	apu->SetMask (0);
	dmc->SetMask (0);

	clocksPerSample = clockHz / rate;
}

NsfApuChip::~NsfApuChip() = default;

void NsfApuChip::write (int address, int value)
{
	if (address < 0x4000 || address > 0x4017)
	{
		return;
	}

	if (address <= 0x4007)
	{
		apu->Write ((UINT32) address, (UINT32) (value & 0xFF));
		return;
	}

	// $4015 enables and silences channels on both halves - the squares' length counters live in one
	// device and the triangle, noise and DPCM in the other, and the tracker's single byte means both.
	if (address == 0x4015)
	{
		apu->Write ((UINT32) address, (UINT32) (value & 0xFF));
	}

	dmc->Write ((UINT32) address, (UINT32) (value & 0xFF));
}

void NsfApuChip::setDpcmRom (const std::uint8_t* bytes, std::size_t length)
{
	rom->bytes.assign (bytes, bytes + length);
}

void NsfApuChip::setMask (int squareMask, int tndMask)
{
	apu->SetMask (squareMask);
	dmc->SetMask (tndMask);
}

void NsfApuChip::render (float* left, float* right, int samples)
{
	for (int i = 0; i < samples; ++i)
	{
		clockAccumulator += clocksPerSample;
		const UINT32 clocks = (UINT32) clockAccumulator;
		clockAccumulator -= (double) clocks;

		apu->Tick (clocks);
		dmc->Tick (clocks);

		INT32 square[2] = { 0, 0 };
		INT32 tnd[2] = { 0, 0 };

		apu->Render (square);
		dmc->Render (tnd);

		// The two halves of the same chip: their outputs are summed, not averaged, because each
		// core renders its own channels at the chip's own level.
		const float mixedLeft = (float) (square[0] + tnd[0]) / fullScale;
		const float mixedRight = (float) (square[1] + tnd[1]) / fullScale;

		if (left != nullptr)
		{
			left[i] += mixedLeft;
		}

		if (right != nullptr)
		{
			right[i] += mixedRight;
		}
	}
}

} // namespace toybox::backends::nes
