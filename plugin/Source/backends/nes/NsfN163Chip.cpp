#include "NsfN163Chip.h"

#include "third_party/nsfplay/xgm/devices/Sound/nes_n106.h"

namespace toybox::backends::nes
{

NsfN163Chip::NsfN163Chip (double sampleRate)
	: chip (new xgm::NES_N106())
{
	chip->SetClock (clockHz);
	chip->SetRate (sampleRate > 0.0 ? sampleRate : 44100.0);
	chip->Reset();

	clocksPerSample = clockHz / (sampleRate > 0.0 ? sampleRate : 44100.0);
}

NsfN163Chip::~NsfN163Chip() = default;

void NsfN163Chip::write (int address, int value)
{
	if (address != 0x4800 && address != 0xF800)
	{
		return;
	}

	chip->Write ((UINT32) address, (UINT32) (value & 0xFF));
}

void NsfN163Chip::render (float* left, float* right, int samples)
{
	for (int i = 0; i < samples; ++i)
	{
		clockAccumulator += clocksPerSample;
		const UINT32 clocks = (UINT32) clockAccumulator;
		clockAccumulator -= (double) clocks;

		chip->Tick (clocks);

		INT32 buffer[2] = { 0, 0 };
		chip->Render (buffer);

		if (left != nullptr)
		{
			left[i] += (float) buffer[0] / fullScale;
		}

		if (right != nullptr)
		{
			right[i] += (float) buffer[1] / fullScale;
		}
	}
}

} // namespace toybox::backends::nes
