#pragma once

#include <memory>
#include <vector>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The NES 2A03: two pulses, triangle, noise and DPCm.

	Unlike a register-shadow decoder this one runs the APU's own frame sequencer, so a note ends
	when the length counter or the envelope actually silences the channel - the reason the old
	one-shot path sounded wrong. Register writes are applied at once, the sequencer advances by
	absolute sample time. */
class NesApuDecoder final : public IChipDecoder
{
public:
	enum Channel
	{
		pulse1 = 0,
		pulse2,
		triangle,
		noise,
		dmc,
		numChannels
	};

	NesApuDecoder();
	~NesApuDecoder() override;

	void reset (std::uint32_t clock, double sampleRate) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	int channelCount() const override { return channelCount_; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	Family family (int channel) const override;
	bool isDrum (int channel) const override { return channel == noise || channel == dmc; }

private:
	struct Envelope
	{
		bool start = false;
		bool loop = false;
		bool constant = false;
		int volume = 0;
		int divider = 0;
		int decay = 0;
	};

	struct LengthCounter
	{
		bool halt = false;
		int value = 0;
	};

	struct Sweep
	{
		bool enabled = false;
		bool negate = false;
		bool reload = false;
		int period = 0;
		int shift = 0;
		int divider = 0;
	};

	struct Pulse
	{
		Envelope envelope;
		LengthCounter length;
		Sweep sweep;
		int timerPeriod = 0;
		int duty = 0;
		int timer = 0;
		bool muted = false;			// sweep silenced it (target period out of range)
	};

	struct Triangle
	{
		LengthCounter length;
		int linearReload = 0;
		int linearCounter = 0;
		bool control = false;		// length halt / linear reload flag
		bool reloadFlag = false;
		int timerPeriod = 0;
		int timer = 0;
	};

	struct NoiseChannel
	{
		Envelope envelope;
		LengthCounter length;
		int periodIndex = 0;
		bool mode = false;
		int timerPeriod = 0;		// CPU cycles from kNoisePeriods
	};

	void stepFrameSequencer (void);
	void clockEnvelope (Envelope& envelope);
	void clockLength (LengthCounter& length);
	void clockSweep (Pulse& pulse);
	void updateGate (void);

	struct Impl;
	std::unique_ptr<Impl> impl;
	int channelCount_ = (int) NesApuDecoder::numChannels;
};

} // namespace toybox::chips
