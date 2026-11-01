#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <typeindex>
#include <vector>

#include "stdtype.h"
#include "player/playera.hpp"

#include "chips/ChipTypes.h"
#include "midi/SongIr.h"

namespace toybox::backends::libvgm
{

/** One decoder's queued writes, type-erased so the router can hold them by decoder type.

	The key is the decoder class rather than the chip kind on purpose: six OPL devices share
	`OplDecoder` and four OPN devices share `OpnDecoder`, so a kind-keyed table would run two
	decoders where the chip is one - fewer notes, no error, the hardest kind of bug to see.
*/
struct DecoderBufferBase
{
	virtual ~DecoderBufferBase() = default;
};

template <typename Decoder>
struct ChipBuffer final : DecoderBufferBase
{
	void queue (const PLR_DEV_INFO* device, uint8_t reg, uint16_t value,
				uint32_t sampleTime, uint8_t port = 0)
	{
		if (device == nullptr)
		{
			return;
		}

		chips::RegisterWrite write;
		write.deviceId = device->id;
		write.reg = reg;
		write.value = value;
		write.port = port;
		write.sampleTime = sampleTime;
		writes[device->id].push_back (write);
		devices[device->id] = device;
	}

	/** The SCC command carries a latch and the value that follows it: the chip sees two writes,
		and so does the decoder. */
	void queuePair (const PLR_DEV_INFO* device, uint8_t reg, uint8_t latch,
					uint8_t value, uint32_t sampleTime, uint8_t port)
	{
		queue (device, reg, latch, sampleTime, port);
		queue (device, (uint8_t) (reg | 1), value, sampleTime, port);
	}

	std::map<uint32_t, std::vector<chips::RegisterWrite>> writes;
	std::map<uint32_t, const PLR_DEV_INFO*> devices;
};

/** The decoders this build can run, created on first use and owned for the router's lifetime.

	Adding a chip is a value in `ChipKind`, a `case` in `ChipWriteRouter::route` and a decoder in
	`chips/` - nothing here has to be extended, which is the point of the registry.
*/
class DecoderRegistry
{
public:
	template <typename Decoder>
	ChipBuffer<Decoder>& decoder (void)
	{
		const auto key = std::type_index (typeid (Decoder));
		const auto found = buffers.find (key);
		if (found != buffers.end())
		{
			return static_cast<ChipBuffer<Decoder>&> (*found->second);
		}

		auto created = std::make_unique<ChipBuffer<Decoder>> ();
		auto* raw = created.get ();
		buffers.emplace (key, std::move (created));
		return *raw;
	}

private:
	std::map<std::type_index, std::unique_ptr<DecoderBufferBase>> buffers;
};

/** Routes one register write to the decoder that owns its chip.

	Two consumers ask this question - the MIDI builder and the visualisation's replay - and the
	answer is knowledge, not a preference: which libvgm device a chip's writes belong to, when a
	port carries a second chip rather than more registers, and which chips have to be split before
	the decoder sees them. Two copies of that would drift, and the symptom would be a row that
	looks merely wrong.

	The router owns the registry and decides where each write goes. Two chips cannot go through it:
	the 2A03 wants one list per device, and the SSG behind an OPN chip has its own clock and its own
	flush. Those arrive through the callbacks below, so that the *reason* they are separate stays
	with the consumer that knows it (the note extractor's) rather than moving in here.
*/
class ChipWriteRouter
{
public:
	/** Copies the device list rather than holding a reference: the map below keeps pointers into
		it, and a copied vector cannot be reallocated by someone else. */
	explicit ChipWriteRouter (const std::vector<PLR_DEV_INFO>& deviceList);

	/** Sends one write where its chip's decoder can see it. */
	void route (const midi::SongIr& ir, const chips::RegisterWrite& write);

	template <typename Decoder>
	ChipBuffer<Decoder>& decoder (void)
	{
		return decoders.decoder<Decoder> ();
	}

	/** The 2A03's writes: one list per device, because its notes end on the length counter rather
		than on a register write. */
	std::function<void (const PLR_DEV_INFO*, uint8_t reg, uint8_t value, uint32_t time)> onNesWrite;

	/** The Game Boy's, for the same reason: a channel's state depends on the sound-enable register
		and on a frame sequencer, neither of which is a single write. */
	std::function<void (const PLR_DEV_INFO*, uint8_t reg, uint8_t value, uint32_t time)> onGbWrite;

	/** The SSG behind an OPN chip: registers 0x00..0x0F on port 0, with its own clock divisor. */
	std::function<void (const PLR_DEV_INFO*, uint8_t reg, uint8_t value, uint32_t time)> onSsgWrite;

	/** The device a libvgm type and instance number name, or the first of that type when the exact
		instance did not start. Null when the song has no such chip. */
	const PLR_DEV_INFO* deviceFor (DEV_ID type, int instance) const;

private:
	/** Both the RF5C68 and the RF5C164 report DEVID_RF5C68; only the config flags tell them apart,
		so 0xB0/0xB1 writes must find the matching variant, not just the first one. */
	const PLR_DEV_INFO* rf5cDeviceFor (bool is164, int instance) const;

	/** The OPN family: port 0's low registers are the AY behind the chip, the rest is FM. */
	void queueOpn (DEV_ID type, int instance, uint8_t reg, uint16_t value,
				   uint32_t time, uint8_t port);

	std::vector<PLR_DEV_INFO> devices;
	std::map<uint64_t, const PLR_DEV_INFO*> deviceByType;
	DecoderRegistry decoders;
};

} // namespace toybox::backends::libvgm
