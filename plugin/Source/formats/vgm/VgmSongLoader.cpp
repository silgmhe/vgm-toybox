#include "VgmSongLoader.h"

#include <cstdint>

#include "backends/libvgm/VgmCommandReader.h"

namespace toybox::formats::vgm
{
namespace
{
	using midi::ChipKind;
	using backends::libvgm::VgmCommand;

	/** The port byte of a VGM command carries two different things depending on the chip: a chip
		instance (0xA0's dual AY, 0xD1's second OPX, 0xE1's second C352), or the high half of a
		16-bit register offset (0xC0, 0xC8, 0xD3, 0xD5). Both end up in `RegisterWrite::port`,
		because both are "the second byte the command carried" - the decoder is what knows which.
		The instance is mirrored into `ChipSlot::instance` so the assembly layer can find the device. */
}

bool VgmSongLoader::load (const void* data, std::size_t size, midi::SongIr& out, std::string& error)
{
	backends::libvgm::VgmCommandReader reader (data, size);
	if (! reader.isValid())
	{
		error = "not a VGM stream";
		return false;
	}

	out = midi::SongIr {};
	out.tickRate = 44100.0;		// VGM's own grid; a CPU-rate format would declare its own here

	// One slot per (chip, instance, variant). The clock is left at zero on purpose: the runtime
	// owns the started devices and knows their clocks, so the assembly layer fills it in.
	const auto slotFor = [&out] (ChipKind kind, int instance, uint32_t flags) -> uint32_t
	{
		for (std::size_t i = 0; i < out.chips.size(); ++i)
		{
			const auto& slot = out.chips[i];
			if (slot.kind == kind && slot.instance == instance && slot.flags == flags)
			{
				return (uint32_t) i;
			}
		}

		midi::ChipSlot slot;
		slot.kind = kind;
		slot.instance = instance;
		slot.flags = flags;
		out.chips.push_back (slot);
		return (uint32_t) (out.chips.size() - 1);
	};

	const auto emit = [&out, &slotFor] (ChipKind kind, int instance, int port,
										uint8_t reg, uint16_t value,
										uint32_t time, uint32_t flags = 0)
	{
		chips::RegisterWrite write;
		write.deviceId = slotFor (kind, instance, flags);
		write.reg = reg;
		write.value = value;
		write.port = (uint8_t) port;
		write.sampleTime = time;
		out.writes.push_back (write);
	};

	VgmCommand command;
	uint32_t lastSampleTime = 0;
	while (reader.next (command))
	{
		lastSampleTime = command.sampleTime;

		const auto port = (int) command.port;
		const auto reg = command.addr;
		const auto value = (uint16_t) command.data;
		const auto time = command.sampleTime;

		switch (command.command)
		{
		// --- PSG and FM, first chip ---------------------------------------------------------
		case 0x50:	emit (ChipKind::sn76489, port, 0, reg, value, time); break;
		case 0x51:	emit (ChipKind::ym2413, 0, 0, reg, value, time); break;
		case 0x52:	emit (ChipKind::ym2612, 0, 0, reg, value, time); break;
		case 0x53:	emit (ChipKind::ym2612, 0, 1, reg, value, time); break;		// port 1 of the pair
		case 0x54:	emit (ChipKind::ym2151, 0, 0, reg, value, time); break;
		case 0x55:	emit (ChipKind::ym2203, 0, 0, reg, value, time); break;
		case 0x56:	emit (ChipKind::ym2608, 0, 0, reg, value, time); break;
		case 0x57:	emit (ChipKind::ym2608, 0, 1, reg, value, time); break;
		case 0x58:	emit (ChipKind::ym2610, 0, 0, reg, value, time); break;
		case 0x59:	emit (ChipKind::ym2610, 0, 1, reg, value, time); break;
		case 0x5A:	emit (ChipKind::ym3812, 0, 0, reg, value, time); break;		// YM3812
		case 0x5B:	emit (ChipKind::ym3526, 0, 0, reg, value, time); break;		// YM3526
		case 0x5C:	emit (ChipKind::y8950, 0, 0, reg, value, time); break;		// Y8950 FM half
		case 0x5D:	emit (ChipKind::ymz280b, 0, 0, reg, value, time); break;
		case 0x5E:	emit (ChipKind::ymf262, 0, 0, reg, value, time); break;		// YMF262
		case 0x5F:	emit (ChipKind::ymf262, 0, 1, reg, value, time); break;

		// --- the same chips, second instance ------------------------------------------------
		case 0xA0:	emit (ChipKind::ay8910, port, 0, reg, value, time); break;
		case 0xA1:	emit (ChipKind::ym2413, 1, 0, reg, value, time); break;
		case 0xA2:	emit (ChipKind::ym2612, 1, 0, reg, value, time); break;
		case 0xA3:	emit (ChipKind::ym2612, 1, 1, reg, value, time); break;
		case 0xA4:	emit (ChipKind::ym2151, 1, 0, reg, value, time); break;
		case 0xA5:	emit (ChipKind::ym2203, 1, 0, reg, value, time); break;
		case 0xA6:	emit (ChipKind::ym2608, 1, 0, reg, value, time); break;
		case 0xA7:	emit (ChipKind::ym2608, 1, 1, reg, value, time); break;
		case 0xA8:	emit (ChipKind::ym2610, 1, 0, reg, value, time); break;
		case 0xA9:	emit (ChipKind::ym2610, 1, 1, reg, value, time); break;
		case 0xAA:	emit (ChipKind::ym3812, 1, 0, reg, value, time); break;
		case 0xAB:	emit (ChipKind::ym3526, 1, 0, reg, value, time); break;
		case 0xAC:	emit (ChipKind::y8950, 1, 0, reg, value, time); break;
		case 0xAE:	emit (ChipKind::ymf262, 1, 0, reg, value, time); break;
		case 0xAF:	emit (ChipKind::ymf262, 1, 1, reg, value, time); break;

		// --- samplers and wavetables --------------------------------------------------------
		case 0xB0:	emit (ChipKind::rf5c68, port, 0, reg, value, time); break;	// RF5C68
		case 0xB1:	emit (ChipKind::rf5c164, port, 0, reg, value, time); break;	// RF5C164
		case 0xB3:	emit (ChipKind::gameBoy, port, 0, reg, value, time); break;
		case 0xB4:	emit (ChipKind::nesApu, port, 0, reg, value, time); break;
		case 0xB5:	emit (ChipKind::multiPcm, 0, 0, reg, value, time); break;
		case 0xB8:	emit (ChipKind::okim6295, port, 0, reg, value, time); break;
		case 0xB9:	emit (ChipKind::huc6280, port, 0, reg, value, time); break;
		case 0xBA:	emit (ChipKind::k053260, 0, 0, reg, value, time); break;
		case 0xBB:	emit (ChipKind::pokey, port, 0, reg, value, time); break;
		case 0xBC:	emit (ChipKind::wonderswan, port, 0, reg, value, time); break;
		case 0xBD:	emit (ChipKind::saa1099, port, 0, reg, value, time); break;
		case 0xBF:	emit (ChipKind::ga20, 0, 0, reg, value, time); break;
		case 0xC0:	emit (ChipKind::segapcm, 0, port, reg, value, time); break;
		case 0xC7:	emit (ChipKind::vsu, port, 0, reg, value, time); break;
		case 0xC8:	emit (ChipKind::x1_010, 0, port, reg, value, time); break;
		case 0xD5:	emit (ChipKind::es5503, port, 0, reg, value, time); break;

		// --- 16-bit offsets and values ------------------------------------------------------
		case 0xC4:	// QSound: value first, then the offset
			emit (ChipKind::qsound, 0, 0, reg,
				  (uint16_t) (((uint16_t) command.data << 8) | command.data2), time);
			break;
		case 0xD3:	// K054539: the offset's high byte rides in `port`
			emit (ChipKind::k054539, 0, port, reg, value, time);
			break;
		case 0xE1:	// C352: a 16-bit offset and a 16-bit value, both big-endian
			emit (ChipKind::c352, (port >> 7) & 1, port & 0x7F, reg,
				  (uint16_t) (((uint16_t) command.data << 8) | command.data2), time);
			break;
		case 0xD4:	// C140 / 219: bit 7 picks the instance, the rest is the offset's high byte
			emit (ChipKind::c140, (port >> 7) & 1, port & 0x7F, reg, value, time,
				  (uint32_t) reader.c140Banking());
			break;

		// --- commands that are not one write ------------------------------------------------
		case 0xD2:	// K051649: one command stands for two registers, `reg` and `reg | 1`
			emit (ChipKind::k051649, port, port, reg, value, time);
			emit (ChipKind::k051649, port, port, (uint8_t) (reg | 1),
				  (uint16_t) command.data2, time);
			break;
		case 0xD0:	// YMF278B: only its OPL3 half (ports 0/1) has a decoder; the wavetable half
					// (ports 2..5) has none, so those writes are dropped here rather than later
			if ((port & 0x7F) < 2)
			{
				emit (ChipKind::ymf278b, (port & 0x80) != 0 ? 1 : 0, port & 0x7F, reg, value, time);
			}
			break;
		case 0xD1:	// YMF271: bit 7 picks the chip, the rest is the bank
			emit (ChipKind::ymf271, (port >> 7) & 1, port & 0x7F, reg, value, time);
			break;

		// --- the C batch --------------------------------------------------------------------
		case 0x40:	emit (ChipKind::mikey, 0, 0, reg, value, time); break;
		case 0x41:	emit (ChipKind::k007232, port, 0, reg, value, time); break;
		case 0x42:	// K005289: Ofs4_Data12 - the register is the command's high nibble
			emit (ChipKind::k005289, 0, 0, reg,
				  (uint16_t) ((((uint16_t) command.data2 & 0x0F) << 8) | command.data), time);
			break;
		case 0x43:	emit (ChipKind::msm5232, 0, 0, reg, value, time); break;

		default:
			break;		// waits, data blocks and stream end are the reader's business
		}
	}

	// The blocks travel as they arrived: which type means which memory, and how a chip folds it
	// into an image, is the chip's business - the same bytes are a 0x8D ROM to a C140 and nothing
	// at all to anything else.
	for (const auto& block : reader.dataBlocks())
	{
		midi::RomBlock rom;
		rom.memoryType = block.type;
		rom.romSize = block.romSize;
		rom.romOffset = block.romOffset;
		rom.ramOffset = block.ramOffset;
		rom.data = block.data;
		rom.size = block.size;
		rom.isRam = (block.type & 0xC0) == 0xC0;
		out.roms.push_back (rom);
	}

	out.totalTicks = lastSampleTime;
	return true;
}

} // namespace toybox::formats::vgm
