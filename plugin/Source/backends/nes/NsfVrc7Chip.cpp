#include "NsfVrc7Chip.h"

#include "third_party/nsfplay/xgm/devices/Sound/nes_vrc7.h"

namespace toybox::backends::nes
{

NsfVrc7Chip::NsfVrc7Chip (double sampleRate)
	: chip (new xgm::NES_VRC7())
{
	chip->SetClock (clockHz);
	chip->SetRate (sampleRate > 0.0 ? sampleRate : 44100.0);
	chip->Reset();

	clocksPerSample = clockHz / (sampleRate > 0.0 ? sampleRate : 44100.0);
}

NsfVrc7Chip::~NsfVrc7Chip() = default;

void NsfVrc7Chip::write (int address, int value)
{
	if (address != 0x9010 && address != 0x9030)
	{
		return;
	}

	chip->Write ((UINT32) address, (UINT32) (value & 0xFF));
}

void NsfVrc7Chip::render (float* left, float* right, int samples)
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
