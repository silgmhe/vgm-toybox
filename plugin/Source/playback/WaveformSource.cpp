#include "playback/WaveformSource.h"

#include "playback/WaveShape.h"

namespace toybox::playback
{

std::vector<float> waveformOf (const ChannelState& channel)
{
	if (channel.noteWaveform != nullptr && ! channel.noteWaveform->empty ())
	{
		return *channel.noteWaveform;
	}

	if (channel.waveValues ().size () >= 2)
	{
		return channel.waveValues ();
	}

	return waveform::cycleFor (channel.waveKind, channel.duty, channel.shapeParameter);
}

} // namespace toybox::playback
