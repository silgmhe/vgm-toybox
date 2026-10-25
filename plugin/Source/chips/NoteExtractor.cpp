#include "NoteExtractor.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace toybox::chips
{
namespace
{
	// 64 samples is ~1.5 ms: finer than a MIDI tick and far cheaper than per-sample polling
	constexpr uint32_t kPollStep = 64;

	// A bend smaller than this is inaudible and would only cost MIDI bandwidth: a shallow
	// vibrato sampled at the poll rate crosses it a few dozen times a second, not hundreds.
	constexpr int kPitchDeadband = 4;

	int velocityFor (int volume)
	{
		const int scaled = (int) std::lround ((double) std::clamp (volume, 0, 15) * 127.0 / 15.0);
		return std::clamp (scaled, 1, 127);
	}
}

ExtractedPerformance extractPerformance (IChipDecoder& decoder,
										 const std::vector<RegisterWrite>& writes,
										 uint32_t totalSamples)
{
	struct OpenNote
	{
		bool active = false;
		int note = -1;
		int fine = 0;
		uint32_t start = 0;
		int velocity = 100;
		int transposeCents = 0;
		int rootCents = 0;
		PitchSource source = PitchSource::unpitched;
	};

	ExtractedPerformance result;
	std::vector<OpenNote> open ((std::size_t) std::max (decoder.channelCount(), 0));

	// The key-on counter of each channel as it was at the last poll: a sampled voice that is
	// retriggered at the same pitch is a new note even though nothing else changed.
	std::vector<uint32_t> lastKeyOns ((std::size_t) std::max (decoder.channelCount(), 0), 0);

	std::size_t nextWrite = 0;

	auto closeNote = [&] (int channel, uint32_t end)
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
			event.note = (uint8_t) current.note;
			event.velocity = (uint8_t) current.velocity;
			event.logicalChannel = channel;
			event.channelKind = decoder.channelKind (channel);
			event.isDrum = decoder.isDrum (channel);
			event.transposeCents = current.transposeCents;
			event.rootCents = current.rootCents;
			event.pitchSource = current.source;
			result.notes.push_back (event);
		}

		current.active = false;
	};

	// a new note always starts on an exact bend, whatever the previous one left behind
	auto openNote = [&] (int channel, uint32_t time, int note, int fine, int volume,
						 const ChannelState& state)
	{
		OpenNote& current = open[(std::size_t) channel];
		current.active = true;
		current.note = note;
		current.fine = fine;
		current.start = time;
		current.velocity = velocityFor (volume);
		current.transposeCents = state.transposeCents;
		current.rootCents = state.rootCents;
		current.source = state.pitchSource;

		PitchEvent bend;
		bend.sampleTime = time;
		bend.logicalChannel = channel;
		bend.cents = (int16_t) std::clamp (fine, -32767, 32767);
		result.bends.push_back (bend);
	};

	const uint32_t lastSample = std::max (totalSamples, 1u);
	for (uint32_t time = 0; time <= lastSample; time += kPollStep)
	{
		while (nextWrite < writes.size() && writes[nextWrite].sampleTime <= time)
		{
			decoder.write (writes[nextWrite]);
			++nextWrite;
		}

		decoder.advance (time);

		for (int channel = 0; channel < decoder.channelCount(); ++channel)
		{
			const ChannelState state = decoder.channelState (channel);

	// A decoder that forgot to fill either pitch camp produces a silent channel rather than an
	// error, so the contract is checked where the state is consumed (debug builds only).
	assert (isPitchStateSane (state));
			const bool retriggered = state.keyOns != lastKeyOns[(std::size_t) channel];
			lastKeyOns[(std::size_t) channel] = state.keyOns;

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
			if (current.active && current.note == note && ! retriggered)
			{
				// same pitch, still sounding: follow the envelope instead of re-triggering
				current.velocity = std::max (current.velocity, velocityFor (state.volume));

				if (std::abs (finePitch - current.fine) >= kPitchDeadband)
				{
					current.fine = finePitch;

					PitchEvent bend;
					bend.sampleTime = time;
					bend.logicalChannel = channel;
					bend.cents = (int16_t) std::clamp (finePitch, -32767, 32767);
					result.bends.push_back (bend);
				}
				continue;
			}

			closeNote (channel, time);
			openNote (channel, time, note, finePitch, state.volume, state);
		}
	}

	for (int channel = 0; channel < decoder.channelCount(); ++channel)
	{
		closeNote (channel, lastSample);
	}

	std::stable_sort (result.notes.begin(), result.notes.end(),
					  [] (const NoteEvent& a, const NoteEvent& b)
					  {
						  return a.startSample < b.startSample;
					  });
	std::stable_sort (result.bends.begin(), result.bends.end(),
					  [] (const PitchEvent& a, const PitchEvent& b)
					  {
						  return a.sampleTime < b.sampleTime;
					  });

	return result;
}

std::vector<NoteEvent> extractNotes (IChipDecoder& decoder,
									 const std::vector<RegisterWrite>& writes,
									 uint32_t totalSamples)
{
	return extractPerformance (decoder, writes, totalSamples).notes;
}

} // namespace toybox::chips
