#pragma once

#include <cstdint>

namespace toybox::playback
{

/** One MIDI message with its offset inside the current audio block. The engine fills this on the
	audio thread; the plugin turns it into the host's MIDI output buffer. */
struct MidiBlockEvent
{
	int sampleOffset = 0;
	std::uint8_t status = 0;
	std::uint8_t data1 = 0;
	std::uint8_t data2 = 0;
};

class MidiBlockSink
{
public:
	virtual ~MidiBlockSink (void) = default;
	virtual void send (const MidiBlockEvent& event) = 0;
};

} // namespace toybox::playback
