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
	re-parsing on every frame is the same work sixty times a second. So the song is parsed once into
	a `SongIr` and this walks it, handing each decoder only the writes it has passed.

	Two facts shape the type. Decoders are per *device*, not per class: four OPN chips share one
	decoder class and must not share an instance, or channel 3 of one song's YM2612 would be the same
	voice as channel 3 of the next. And the routing is the router's - which chip a write belongs to
	and which decoder answers it is exactly the question `ChipWriteRouter` exists to answer once.

	Audio thread. `advanceTo` allocates only when a device appears that it has not seen before, which
	is the first frame after a song is opened; after that it is a walk over two maps.
*/
class VgmReplayer
{
public:
	/** One decoder instance, and the device it is the state of. */
	struct Instance
	{
		// A constructor rather than aggregate initialisation: `std::type_index` has no default, so
		// the members cannot carry initialisers and still form an aggregate.
		Instance (std::type_index decoderClass, uint32_t id, const PLR_DEV_INFO* dev,
				  chips::IChipDecoder* dec, uint32_t clk)
			: type (decoderClass), deviceId (id), device (dev), decoder (dec), clock (clk)
		{
		}

		std::type_index type;
		uint32_t deviceId;
		const PLR_DEV_INFO* device;
		chips::IChipDecoder* decoder;
		uint32_t clock;		// kept for reset(): a rewind must not fall back to the chip's default
	};

	VgmReplayer (std::shared_ptr<const midi::SongIr> song, const std::vector<PLR_DEV_INFO>& devices);

	/** Routes and feeds every write up to `sampleTime`, then moves the decoders to it.

		Writes are consumed as they are fed, so a second call at the same position does nothing.
		Going backwards is not supported - `rewind()` is the way to start over, and the caller knows
		when that is right (a loop, a seek) while this class does not.
	*/
	void advanceTo (uint32_t sampleTime);

	/** Back to the start of the song: every decoder reset, every queued write dropped, the cursor
		back at the first write.

		The alternative - leaving the decoders as they are and letting the cursor wrap - is what
		produces a snapshot describing the position before a loop point while the music plays the one
		after it.
	*/
	void rewind (void);

	/** The live instances, for a caller that reads `channelState` from each. */
	const std::vector<Instance>& instances (void) const { return live; }

	/** The unit `advanceTo` takes, from the song's loader. */
	double tickRate (void) const;

private:
	/** Creates an instance for any device the router has delivered writes for since the last call. */
	void adoptNewBuffers (void);

	std::shared_ptr<const midi::SongIr> song;
	ChipWriteRouter router;
	std::size_t nextWrite = 0;

	std::vector<Instance> live;
	std::vector<std::unique_ptr<chips::IChipDecoder>> owned;
};

} // namespace toybox::backends::libvgm
