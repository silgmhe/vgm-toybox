#pragma once

#include <cstdint>
#include <memory>
#include <typeindex>
#include <vector>

#include "stdtype.h"
#include "player/playera.hpp"

#include "chips/IChipDecoder.h"
#include "midi/SongIr.h"

#include "ChipWriteRouter.h"

namespace toybox::backends::libvgm
{

/**
	Drives the register decoders in step with playback, instead of once for a whole song.

	The MIDI builder reads the entire stream before it makes a single note, because a note's length
	only exists after the song has ended. A display cannot do that: it needs the state *now*, and
	re-parsing on every frame is the same work sixty times a second. So this holds one decoder per
	(decoder class, device) for the song's lifetime and feeds it only the writes it has passed.

	Two facts shape the type. Decoders are per *device*, not per class: four OPN devices share one
	decoder class and must not share an instance, or channel 3 of one chip and channel 3 of the next
	would be the same voice. And the routing is the router's, not this class's - which chip a write
	belongs to and which decoder answers it is exactly the question `ChipWriteRouter` exists to
	answer once.

	Message-thread only, except where a caller says otherwise: `advanceTo` is written to run on the
	audio thread inside `captureState`, where it touches nothing shared.
*/
class VgmReplayer
{
public:
	/** One decoder instance, and the device it is the state of. */
	struct Instance
	{
		std::type_index type;
		uint32_t deviceId = chips::kNoDevice;
		const PLR_DEV_INFO* device = nullptr;
		chips::IChipDecoder* decoder = nullptr;
		uint32_t clock = 0;		// kept for reset(): a rewind must not fall back to the chip's default
	};

	VgmReplayer (const std::vector<PLR_DEV_INFO>& devices, double tickRate);

	/** Feeds every write up to `sampleTime` and moves the decoders to it.

		Writes are consumed as they are fed, so a second call with the same time does nothing. Going
		backwards is not supported - `rewind()` is the way to start over, and the caller knows when
		that is right (a loop, a seek) while this class does not.
	*/
	void advanceTo (uint32_t sampleTime);

	/** Back to the start of the song: every decoder reset, every queued write forgotten.

		The alternative - leaving the decoders as they are and letting the cursor wrap - is what
		produces a snapshot that describes the position before a loop point while the music plays the
		one after it.
	*/
	void rewind (void);

	/** The live instances, for a caller that reads `channelState` from each. */
	const std::vector<Instance>& instances (void) const { return live; }

	/** Where the decoders have been fed up to, in the song's own tick units. */
	uint32_t position (void) const { return fedTo; }

private:
	/** Creates an instance for any buffer that has appeared since the last call. */
	void adoptNewBuffers (void);

	ChipWriteRouter router;
	double tickRate = 44100.0;
	std::vector<Instance> live;
	std::vector<std::unique_ptr<chips::IChipDecoder>> owned;
	uint32_t fedTo = 0;
	bool started = false;
};

} // namespace toybox::backends::libvgm
