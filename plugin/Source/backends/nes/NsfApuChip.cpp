#include "NsfApuChip.h"

#include <algorithm>
#include <cmath>

#include "nes_apu.h"

namespace toybox::backends::nes
{

NsfApuChip::NsfApuChip (double sampleRate)
	: apu (new xgm::NES_APU())
{
	apu->SetClock (clockHz);
	apu->SetRate (sampleRate > 0.0 ? sampleRate : 44100.0);
	apu->Reset();

	clocksPerSample = clockHz / (sampleRate > 0.0 ? sampleRate : 44100.0);
}

NsfApuChip::~NsfApuChip() = default;

void NsfApuChip::write (int address, int value)
{
	if (address < 0x4000 || address > 0x4017)
	{
		return;
	}

	apu->Write ((UINT32) address, (UINT32) (value & 0xFF));
}

void NsfApuChip::render (float* left, float* right, int samples)
{
	for (int i = 0; i < samples; ++i)
	{
		// The core is clocked, not sampled: it advances by whole clocks and the remainder carries,
		// which is what keeps a 1.79 MHz chip in step with a 44.1 kHz output over a long song.
		clockAccumulator += clocksPerSample;
		const UINT32 clocks = (UINT32) clockAccumulator;
		clockAccumulator -= (double) clocks;

		apu->Tick (clocks);

		INT32 buffer[2] = { 0, 0 };
		apu->Render (buffer);

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
