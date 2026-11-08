#include "VgmReplayer.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>

#include "ChipMemoryAssembly.h"

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

			// The device's own rate travels with the config: a decoder whose sample rate is a clock
			// divided by a board strap cannot know the strap, and the device was started with the
			// answer. Zero for the devices that never asked for it.
			const double deviceRate = (device != nullptr) ? (double) device->smplRate : 0.0;

			decoder->reset (chips::ChipConfig { clock, tickRate (), 0, deviceRate });

			// The song's own blocks, assembled the way the MIDI path assembles them. Without this a
			// sampled chip measures nothing: the C352's pitch is its rate divided by a cycle length
			// that only exists in the ROM, so an empty image puts every voice at C9 - a number that
			// looks like a note and belongs to no sound.
			std::vector<uint8_t> image;
			uint8_t banking = 0;

			if (requirements.hasMemory && song != nullptr)
			{
				image = memory::forRequirements (*song, requirements, banking);

				if (! image.empty ())
				{
					chips::ChipMemory attached;
					attached.bytes = &image;
					attached.banking = banking;
					decoder->attachMemory (attached);	// the decoder copies what it needs
				}
			}

			live.push_back (Instance { type, deviceId, device, decoder.get (), clock,
									   std::move (image), banking });
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

	// **[TOYBOX PATCH] Advance every decoder to the capture time, not just to the last write.**

	// `advance()` used to be called only with a write's own `sampleTime` (the loop above), so a chip
	// that needs the passage of time - the YMZ280B row shows the slice being read - froze as soon as
	// the last write of a section had gone past: the probe measured the identical window at t=5 s and
	// t=15 s, because `now` never moved again. The target time is known right here, so pass it on.
	// Decoders that do not use `advance()` ignore it, which is most of them.
	for (auto& instance : live)
	{
		if (instance.decoder != nullptr)
		{
			instance.decoder->advance (sampleTime);
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

		// `reset` rebuilds the decoder, so the image it was given is gone with the old state. A
		// rewind that left it out would make every sample unreadable from the second pass on -
		// and the rows would keep their notes, because those come from the registers.
		if (! instance.image.empty ())
		{
			chips::ChipMemory attached;
			attached.bytes = &instance.image;
			attached.banking = instance.banking;
			instance.decoder->attachMemory (attached);
		}
	}

	router.clearWrites ();
	nextWrite = 0;
}

} // namespace toybox::backends::libvgm
