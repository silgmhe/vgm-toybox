#include "NoteExtractor.h"

#include <algorithm>
#include <cmath>

namespace toybox::chips
{
namespace
{
	// 64 samples is ~1.5 ms: finer than a MIDI tick and far cheaper than per-sample polling
	constexpr std::uint32_t kPollStep = 64;

	int velocityFor (int volume)
	{
		const int scaled = (int) std::lround ((double) std::clamp (volume, 0, 15) * 127.0 / 15.0);
		return std::clamp (scaled, 1, 127);
	}
}

std::vector<NoteEvent> extractNotes (IChipDecoder& decoder,
									 const std::vector<RegisterWrite>& writes,
									 std::uint32_t totalSamples)
{
	struct OpenNote
	{
		bool active = false;
		int note = -1;
		std::uint32_t start = 0;
		int velocity = 100;
	};

	std::vector<NoteEvent> notes;
	const int channels = decoder.channelCount();
	std::vector<OpenNote> open ((std::size_t) std::max (channels, 0));

	std::size_t nextWrite = 0;

	auto closeNote = [&] (int channel, std::uint32_t end)
	{
		OpenNote& current = open[(std::size_t) channel];
		if (! current.active)
		{
			return;
		}

		if (current.note >= 0 && end > current.start)
		{
			NoteEvent event;
			event.startSample = current.start;
			event.endSample = end;
			event.note = (std::uint8_t) current.note;
			event.velocity = (std::uint8_t) current.velocity;
			event.logicalChannel = channel;
			event.family = decoder.family (channel);
			event.isDrum = decoder.isDrum (channel);
			notes.push_back (event);
		}

		current.active = false;
	};

	const std::uint32_t lastSample = std::max (totalSamples, 1u);
	for (std::uint32_t time = 0; time <= lastSample; time += kPollStep)
	{
		while (nextWrite < writes.size() && writes[nextWrite].sampleTime <= time)
		{
			decoder.write (writes[nextWrite]);
			++nextWrite;
		}

		decoder.advance (time);

		for (int channel = 0; channel < channels; ++channel)
		{
			const ChannelState state = decoder.channelState (channel);
			int finePitch = 0;
			const int note = (state.present && state.gate)
								 ? decoder.noteTable (channel).noteFor (state.period, state.block, &finePitch)
								 : -1;

			if (note < 0)
			{
				closeNote (channel, time);
				continue;
			}

			OpenNote& current = open[(std::size_t) channel];
			if (current.active && current.note == note)
			{
				// same pitch, still sounding: follow the envelope instead of re-triggering
				current.velocity = std::max (current.velocity, velocityFor (state.volume));
				continue;
			}

			closeNote (channel, time);
			current.active = true;
			current.note = note;
			current.start = time;
			current.velocity = velocityFor (state.volume);
		}
	}

	for (int channel = 0; channel < channels; ++channel)
	{
		closeNote (channel, lastSample);
	}

	std::stable_sort (notes.begin(), notes.end(), [] (const NoteEvent& a, const NoteEvent& b)
	{
		return a.startSample < b.startSample;
	});

	return notes;
}

} // namespace toybox::chips
