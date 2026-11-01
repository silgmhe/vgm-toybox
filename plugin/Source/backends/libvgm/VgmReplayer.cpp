#include "VgmReplayer.h"

#include <algorithm>

namespace toybox::backends::libvgm
{

VgmReplayer::VgmReplayer (const std::vector<PLR_DEV_INFO>& devices, double rate)
	: router (devices), tickRate (rate > 0.0 ? rate : 44100.0)
{
}

void VgmReplayer::adoptNewBuffers (void)
{
	for (auto* buffer : router.buffers())
	{
		const auto type = buffer->decoderType ();

		// A device the router has already delivered writes for. One instance per device: the same
		// decoder class drives four OPN chips and must not be one object between them.
		for (const auto& entry : buffer->devices)
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

			// No memory image yet. The first version of the visualisation draws every channel's
			// shape from its kind, and the sampled chips need their ROM before a note means
			// anything - see docs/desgin/libvgm-visualisation.md, limitation 1.
			decoder->reset (chips::ChipConfig { clock, tickRate });

			Instance instance;
			instance.type = type;
			instance.deviceId = deviceId;
			instance.device = device;
			instance.decoder = decoder.get ();
			instance.clock = clock;

			live.push_back (instance);
			owned.push_back (std::move (decoder));
		}
	}
}

void VgmReplayer::advanceTo (uint32_t sampleTime)
{
	adoptNewBuffers ();

	for (auto* buffer : router.buffers())
	{
		const auto type = buffer->decoderType ();

		for (auto& entry : buffer->writes)
		{
			auto& writes = entry.second;

			const auto found = std::find_if (live.begin(), live.end(),
											 [type, deviceId = entry.first] (const Instance& instance)
											 {
												 return instance.type == type && instance.deviceId == deviceId;
											 });

			if (found == live.end() || found->decoder == nullptr)
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

	fedTo = sampleTime;
	started = true;
}

void VgmReplayer::rewind (void)
{
	// Every decoder goes back to its reset state, and every write that has been fed is dropped by
	// the caller's own queue - this class cannot un-feed what it has already written.
	//
	// The clock comes from the instance rather than being left at zero: a decoder's reset falls back
	// to its own default when the clock is unknown, and that default is not this device's clock, so
	// a rewind would quietly retune every chip that reports one.
	for (auto& instance : live)
	{
		instance.decoder->reset (chips::ChipConfig { instance.clock, tickRate });
	}

	fedTo = 0;
	started = false;
}

} // namespace toybox::backends::libvgm
