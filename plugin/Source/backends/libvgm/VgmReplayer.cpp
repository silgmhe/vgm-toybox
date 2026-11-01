#include "VgmReplayer.h"

#include <algorithm>

#include "chips/Ay8910Decoder.h"
#include "chips/GbcApuDecoder.h"
#include "chips/NesApuDecoder.h"

namespace toybox::backends::libvgm
{

VgmReplayer::VgmReplayer (std::shared_ptr<const midi::SongIr> ir,
						   const std::vector<PLR_DEV_INFO>& devices)
	: song (std::move (ir)), router (devices)
{
	// The three chips whose writes the router hands to a callback rather than queueing itself. The
	// MIDI path keeps them apart because a note's length only exists once the whole song has been
	// read; a display wants "what is this channel doing now", so here they simply join the registry
	// like every other chip and get an instance the same way.
	//
	// Leaving these unset is not a slow path - it is a silent one: the router drops the write, no
	// buffer is ever created for the chip, and the page shows nothing while the music plays.
	router.onNesWrite = [this] (const PLR_DEV_INFO* device, uint8_t reg, uint8_t value, uint32_t time)
	{
		router.decoder<chips::NesApuDecoder> ().queue (device, reg, value, time);
	};

	router.onGbWrite = [this] (const PLR_DEV_INFO* device, uint8_t reg, uint8_t value, uint32_t time)
	{
		router.decoder<chips::GbcApuDecoder> ().queue (device, reg, value, time);
	};

	router.onSsgWrite = [this] (const PLR_DEV_INFO* device, uint8_t reg, uint8_t value, uint32_t time)
	{
		router.decoder<chips::Ay8910Decoder> ().queue (device, reg, value, time);
	};
}

double VgmReplayer::tickRate (void) const
{
	return song != nullptr && song->tickRate > 0.0 ? song->tickRate : 44100.0;
}

void VgmReplayer::adoptNewBuffers (void)
{
	for (auto* buffer : router.buffers())
	{
		const auto type = buffer->decoderType ();

		for (const auto& entry : buffer->deviceTable ())
		{
			const auto deviceId = entry.first;

			const bool known = std::any_of (live.begin(), live.end(),
											[type, deviceId] (const Instance& instance)
											{
												return instance.type == type && instance.deviceId == deviceId;
											});

			if (known)
			{
				continue;
			}

			auto decoder = buffer->makeDecoder ();
			if (decoder == nullptr)
			{
				continue;
			}

			const auto requirements = buffer->requirements ();
			const auto* device = entry.second;
			const uint32_t clock = (device != nullptr && device->devCfg != nullptr && device->devCfg->clock > 0)
									   ? device->devCfg->clock
									   : requirements.fallbackClock;

			// No memory image yet. The first version draws every channel's shape from its kind, and
			// a sampled chip needs its ROM before a note means anything - see
			// docs/desgin/libvgm-visualisation.md, limitation 1.
			decoder->reset (chips::ChipConfig { clock, tickRate () });

			live.push_back (Instance { type, deviceId, device, decoder.get (), clock });
			owned.push_back (std::move (decoder));
		}
	}
}

void VgmReplayer::advanceTo (uint32_t sampleTime)
{
	if (song == nullptr)
	{
		return;
	}

	// Route first: the router decides which decoder a write belongs to and queues it there. Doing it
	// here rather than in the caller keeps the cursor and the queue in one place - the two have to
	// agree, and a caller that stopped routing halfway would leave the decoders behind silently.
	for (; nextWrite < song->writes.size (); ++nextWrite)
	{
		const auto& write = song->writes[nextWrite];

		if (write.sampleTime > sampleTime)
		{
			break;
		}

		router.route (*song, write);
	}

	adoptNewBuffers ();

	for (auto* buffer : router.buffers())
	{
		const auto type = buffer->decoderType ();

		for (auto& entry : buffer->pendingWrites ())
		{
			auto& writes = entry.second;

			const auto found = std::find_if (live.begin(), live.end(),
											 [type, deviceId = entry.first] (const Instance& instance)
											 {
												 return instance.type == type && instance.deviceId == deviceId;
											 });

			if (found == live.end () || found->decoder == nullptr)
			{
				continue;		// a device with no decoder: its writes are still ours to drop
			}

			auto* decoder = found->decoder;

			// The vector is in stream order, so the first write past the target means every write
			// after it is past it too.
			std::size_t consumed = 0;

			for (; consumed < writes.size (); ++consumed)
			{
				const auto& write = writes[consumed];

				if (write.sampleTime > sampleTime)
				{
					break;
				}

				decoder->write (write);
				decoder->advance (write.sampleTime);
			}

			writes.erase (writes.begin (), writes.begin () + (std::ptrdiff_t) consumed);
		}
	}
}

void VgmReplayer::rewind (void)
{
	// Every decoder goes back to its reset state, and every write that has been routed but not fed
	// is dropped: leaving them queued would feed the end of the song's writes into a decoder that
	// has just been put back at the start.
	for (auto& instance : live)
	{
		// The clock comes from the instance rather than being left at zero: a decoder's reset falls
		// back to its own default when the clock is unknown, and that default is not this device's,
		// so a rewind would quietly retune every chip that reports one.
		instance.decoder->reset (chips::ChipConfig { instance.clock, tickRate () });
	}

	router.clearWrites ();
	nextWrite = 0;
}

} // namespace toybox::backends::libvgm
