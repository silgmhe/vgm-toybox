#include "VgmReplayer.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>

#include "ChipMemoryAssembly.h"
#include "emu/SoundDevs.h"

#include "chips/Ay8910Decoder.h"
#include "chips/GbcApuDecoder.h"
#include "chips/NesApuDecoder.h"
#include "chips/OplDecoder.h"
#include "chips/OpnDecoder.h"

#include "emu/cores/fmopn_2608rom.h"

namespace
{
	/** **The ordinal `ChipConfig::flags` carries for an OPN variant.**

		`OpnDecoder::reset` reads it back as `Variant (flags - 1)`, so this is that expression from
		the other end. Written once instead of as four literals in the device `switch` plus a fifth
		in the rhythm-ROM check: adding a variant there means adding its enum value, not working out
		its position again.
	*/
	constexpr uint32_t opnSlotFlag (toybox::chips::OpnDecoder::Variant variant)
	{
		return (uint32_t) variant + 1u;
	}

	/** The YM2608, which is the only OPN whose rhythm bank lives inside the part. */
	constexpr uint32_t kOpnaSlotFlag = opnSlotFlag (toybox::chips::OpnDecoder::Variant::ym2608);

	/** **The OPNA's rhythm samples are in the chip, not in the song.**

		Its six ADPCM-A voices read a 16 KB ROM the core carries as `YM2608_ADPCM_ROM`; no VGM block
		holds it. `kRequirements` therefore asks for `0x82` - the OPNB's separate bank - which a
		YM2608 file does not have, so the decoder's bank stayed empty, the six rows drew nothing,
		and the mixer played them anyway.

		Handed over from here rather than included in `OpnDecoder.cpp`: `vgm_chip_test` compiles that
		file without libvgm's include path, so pulling this header into it would break the target.

		`2` is the ordinal `VgmReplayer` already puts in `ChipConfig::flags` for a YM2608.
	*/
	void attachRhythmRom (toybox::chips::IChipDecoder& decoder, uint8_t slotFlags)
	{
		if (slotFlags != kOpnaSlotFlag || typeid (decoder) != typeid (toybox::chips::OpnDecoder))
		{
			return;
		}

		static const std::vector<uint8_t> rhythm (YM2608_ADPCM_ROM,
												  YM2608_ADPCM_ROM + sizeof (YM2608_ADPCM_ROM));

		toybox::chips::ChipMemory memory;
		memory.bytes = &rhythm;
		decoder.attachMemory (memory);
	}
}

namespace toybox::backends::libvgm
{

VgmReplayer::VgmReplayer (std::shared_ptr<const midi::SongIr> ir,
						   const std::vector<PLR_DEV_INFO>& devices, RomCache* cache)
	: song (std::move (ir)), romCache (cache), router (devices)
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

			// **The chip slot's flags reach the decoder here.** The SegaPCM's bank shift and mask
			// live in the VGM header and the two songs in this collection disagree (13/0xF8 against
			// 12/0x70); without them a decoder reads the right page out of the wrong bank, which
			// draws a flat line and exports silence.
			uint32_t slotFlags = 0;

			// **An OPN device has to say which OPN it is.** Four devices share `OpnDecoder`, and the
			// buffer makes its instances with the default constructor - the YM2612 layout of six FM
			// plus a DAC channel. A YM2610 or YM2608 therefore reported seven channels where the
			// chip has thirteen, and its ADPCM-A and ADPCM-B voices had no row at all: the notes
			// were in the music, the table just never listed them. The variant travels as
			// `ChipConfig::flags` (1 = ym2203, 2 = ym2608, 3 = ym2610, 4 = ym2610b, 5 = ym2612).
			if (device != nullptr)
			{
				switch (device->type)
				{
				case DEVID_YM2203: slotFlags = opnSlotFlag (chips::OpnDecoder::Variant::ym2203); break;
				case DEVID_YM2608: slotFlags = kOpnaSlotFlag; break;
				case DEVID_YM2610: slotFlags = opnSlotFlag (chips::OpnDecoder::Variant::ym2610); break;
				case DEVID_YM2612: slotFlags = opnSlotFlag (chips::OpnDecoder::Variant::ym2612); break;

				// **The OPL family shares one decoder, and which member it is has to be said too.**
				// It arrives as the default `opl2`, so a YM2413 was decoded through OPL2's register
				// map - `0xA0`/`0xB0` for pitch and key-on, where an OPLL writes `0x10..0x18` and
				// `0x20..0x28`. Its rows therefore had no period, no block and no gate: no note in
				// the keyboard row and no key highlighted, while the MIDI was correct because
				// `MidiSequenceBuilder` builds a decoder of its own with the variant it works out.
				// The OPL3s lost the second nine-channel bank the same way.
				case DEVID_YM2413:
					slotFlags = chips::OplDecoder::slotFlag (chips::OplDecoder::Variant::opll);
					break;

				case DEVID_YMF262:
				case DEVID_YMF278B:
					slotFlags = chips::OplDecoder::slotFlag (chips::OplDecoder::Variant::opl3);
					break;

				// Named rather than left to the default, so the set of variants the replayer can
				// produce is visible in one place.
				case DEVID_YM3812:
				case DEVID_YM3526:
					slotFlags = chips::OplDecoder::slotFlag (chips::OplDecoder::Variant::opl2);
					break;

				// Only the Y8950 has a Delta-T unit: bit 0 says so, which is what gives it a
				// fifteenth row (and the ROM it draws from).
				case DEVID_Y8950:
					slotFlags = chips::OplDecoder::deltaTFlag
								| chips::OplDecoder::slotFlag (chips::OplDecoder::Variant::opl2);
					break;

				// **SCC or SCC+.** The VGM clock field's bit 31 selects the second one, and the two
				// address their waveform RAM differently: SCC shares one table between channels 3
				// and 4, SCC+ gives channel 4 its own half. Without this flag an SCC+ song draws
				// channel 4 with channel 3's shape.
				case DEVID_K051649:
					slotFlags = (device->devCfg != nullptr) ? (device->devCfg->flags & 1u) : 0u;
					break;

				default: break;
				}
			}

			if (slotFlags == 0 && song != nullptr)
			{
				for (const auto& slot : song->chips)
				{
					if (slot.kind == midi::ChipKind::segapcm)
					{
						slotFlags = slot.flags;
					}
				}
			}

			decoder->reset (chips::ChipConfig { clock, tickRate (), slotFlags, deviceRate });

			// The song's own blocks, assembled the way the MIDI path assembles them. Without this a
			// sampled chip measures nothing: the C352's pitch is its rate divided by a cycle length
			// that only exists in the ROM, so an empty image puts every voice at C9 - a number that
			// looks like a note and belongs to no sound.
			// **Shared, because the bytes belong to the chip and not to this player.** Assembled
			// once per device type and handed to every player that reads it: a Neo Geo's eight
			// megabyte image used to be copied once per mixer row, which is where most of a
			// sixteen-row song's memory went. See `RomCache`.
			std::shared_ptr<const std::vector<uint8_t>> image;
			uint8_t banking = 0;

			if (requirements.hasMemory && song != nullptr)
			{
				if (romCache != nullptr)
				{
					if (const auto* cached = romCache->find (deviceId))
					{
						image = cached->image;
						banking = cached->banking;
					}
				}

				if (image == nullptr)
				{
					std::vector<uint8_t> fresh = memory::forRequirements (*song, requirements, banking);

					if (! fresh.empty ())
					{
						image = std::make_shared<const std::vector<uint8_t>> (std::move (fresh));
					}

					if (romCache != nullptr && image != nullptr)
					{
						romCache->add (deviceId, image, banking);
					}
				}

				if (image != nullptr && ! image->empty ())
				{
					chips::ChipMemory attached;
					attached.bytes = image.get ();
					attached.banking = banking;
					decoder->attachMemory (attached);	// the decoder copies what it needs
				}

				attachRhythmRom (*decoder, slotFlags);

				// **The images a decoder asked for beyond the first.** The OPN family wants its Delta-T
				// ROM as well as the ADPCM-A bank, and VGM numbers that block per board (`0x83` on the
				// OPNB, `0x81` on the OPNA) - so the decoder names both types and this serves whichever
				// the song carries, tagged so the decoder can tell them apart.
				const uint8_t extraTypes[2] = { requirements.secondaryMemoryType,
												requirements.alternateMemoryType };
				const std::size_t extraBytes[2] = { requirements.secondaryMemoryBytes,
													requirements.alternateMemoryBytes };

				for (int extra = 0; extra < 2; ++extra)
				{
					if (extraTypes[extra] == 0)
					{
						continue;
					}

					const std::vector<uint8_t> secondary = memory::rom (*song, extraTypes[extra],
																		extraBytes[extra], false);

					if (secondary.empty ())
					{
						continue;
					}

					chips::ChipMemory attached;
					attached.bytes = &secondary;
					attached.type = extraTypes[extra];
					decoder->attachSecondaryMemory (attached);
				}
			}

			live.push_back (Instance { type, deviceId, device, decoder.get (), clock,
									   image, banking, slotFlags });
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
		instance.decoder->reset (chips::ChipConfig { instance.clock, tickRate (), instance.flags });

		// `reset` rebuilds the decoder, so the image it was given is gone with the old state. A
		// rewind that left it out would make every sample unreadable from the second pass on -
		// and the rows would keep their notes, because those come from the registers.
		if (instance.image != nullptr && ! instance.image->empty ())
		{
			chips::ChipMemory attached;
			attached.bytes = instance.image.get ();
			attached.banking = instance.banking;
			instance.decoder->attachMemory (attached);
		}

		attachRhythmRom (*instance.decoder, instance.flags);
	}

	router.clearWrites ();
	nextWrite = 0;
}

} // namespace toybox::backends::libvgm
