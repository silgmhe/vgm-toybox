#include "VgmCommandReader.h"

#include <cstring>

namespace toybox::backends::libvgm
{
namespace
{
	std::uint16_t readLe16 (const std::uint8_t* p)
	{
		return (std::uint16_t) ((std::uint16_t) p[0] | ((std::uint16_t) p[1] << 8));
	}

	std::uint32_t readLe32 (const std::uint8_t* p)
	{
		return (std::uint32_t) p[0] | ((std::uint32_t) p[1] << 8) | ((std::uint32_t) p[2] << 16)
			   | ((std::uint32_t) p[3] << 24);
	}

	/** Length of one command in the stream, header byte included. Command lengths are taken
		from libvgm's VGM command table: a wrong length desynchronises the whole stream, so the
		rarely used commands are listed too. */
	int commandLength (std::uint8_t cmd, const std::uint8_t* data, std::size_t remaining)
	{
		switch (cmd)
		{
		case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
		case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B:
		case 0x3C: case 0x3D: case 0x3E: case 0x3F:
		case 0x4F:
		case 0x50:
			return 2;
		case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45:
		case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B:
		case 0x4C: case 0x4D: case 0x4E:
		case 0x51: case 0x52: case 0x53: case 0x54: case 0x55:
		case 0x56: case 0x57: case 0x58: case 0x59:
		case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
		case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5:
		case 0xA6: case 0xA7: case 0xA8: case 0xA9: case 0xAA: case 0xAB:
		case 0xAC: case 0xAD: case 0xAE: case 0xAF:
		case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5:
		case 0xB6: case 0xB7: case 0xB8: case 0xB9: case 0xBA: case 0xBB:
		case 0xBC: case 0xBD: case 0xBE: case 0xBF:
		case 0x61:
			return 3;
		case 0x62: case 0x63:
		case 0x66:
		case 0x70: case 0x71: case 0x72: case 0x73:
		case 0x74: case 0x75: case 0x76: case 0x77:
		case 0x78: case 0x79: case 0x7A: case 0x7B:
		case 0x7C: case 0x7D: case 0x7E: case 0x7F:
			return 1;
		case 0x67:
			// bit 31 marks a ROM data block; the length itself is the low 31 bits
			return remaining >= 7 ? 7 + (int) (readLe32 (data + 3) & 0x7FFFFFFFu)
								  : (int) remaining;
		case 0x68:
			return 12;
		case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
		case 0x86: case 0x87: case 0x88: case 0x89: case 0x8A: case 0x8B:
		case 0x8C: case 0x8D: case 0x8E: case 0x8F:
			return 1;
		case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5:
		case 0xC6: case 0xC7: case 0xC8: case 0xC9: case 0xCA: case 0xCB:
		case 0xCC: case 0xCD: case 0xCE: case 0xCF:
		case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xD4: case 0xD5:
		case 0xD6: case 0xD7: case 0xD8: case 0xD9: case 0xDA: case 0xDB:
		case 0xDC: case 0xDD: case 0xDE: case 0xDF:
			return 4;
		case 0x90: case 0x91:
		case 0x95:
		case 0xE0: case 0xE1:
			return 5;
		case 0x92:
			return 6;
		case 0x93:
			return 11;
		case 0x94:
			return 2;
		case 0x96: case 0x97: case 0x98: case 0x99: case 0x9A: case 0x9B:
		case 0x9C: case 0x9D: case 0x9E: case 0x9F:
			return 1;
		default:
			return cmd >= 0xE2 ? 5 : 1;
		}
	}

	/** Commands whose address byte carries the second-chip flag in bit 7. */
	bool splitsAddressByte (std::uint8_t cmd)
	{
		return cmd == 0xB0 || cmd == 0xB1 || cmd == 0xB3 || cmd == 0xB4
			   || cmd == 0xB9 || cmd == 0xBB || cmd == 0xBC || cmd == 0xBD;
	}

	/** The WonderSwan keeps its sound registers at 0x80..: the command stores them from 0x00. */
	std::uint8_t registerAddress (std::uint8_t cmd, std::uint8_t offset)
	{
		return (cmd == 0xBC) ? (std::uint8_t) (0x80 + offset) : offset;
	}
}

VgmCommandReader::VgmCommandReader (const void* image, std::size_t imageSize)
	: data (static_cast<const std::uint8_t*> (image)), size (imageSize)
{
	if (! readHeader())
	{
		position = end = 0;
	}
}

bool VgmCommandReader::readHeader (void)
{
	if (data == nullptr || size < 0x40 || std::memcmp (data, "Vgm ", 4) != 0)
	{
		return false;
	}

	std::uint32_t dataOffset = 0x40;
	const std::uint32_t relativeData = readLe32 (data + 0x34);
	if (relativeData != 0)
	{
		dataOffset = 0x34 + relativeData;
	}

	end = readLe32 (data + 0x04);
	end = (end == 0) ? size : (0x04 + end);
	if (end > size)
	{
		end = size;
	}

	// like libvgm: the GD3 tag sits between the command stream and the EOF offset, so the
	// command stream ends there (parsing the tag as commands would desync)
	const std::uint32_t relativeGd3 = readLe32 (data + 0x14);
	if (relativeGd3 != 0)
	{
		const std::uint32_t gd3Offset = 0x14 + relativeGd3;
		if (gd3Offset >= dataOffset && gd3Offset < end)
		{
			end = gd3Offset;
		}
	}

	if (dataOffset < 0x40 || dataOffset >= end)
	{
		return false;
	}

	// the C140's banking is a header field, not a command: byte 0x96, added with the 1.61 header.
	// A shorter header has a command there, so only read it when the field is really inside the
	// header - libvgm does the same and otherwise leaves the default (System 2) in place.
	if (dataOffset > 0x96 && size > 0x96)
	{
		c140BankingType = data[0x96];
	}

	position = dataOffset;
	return true;
}

bool VgmCommandReader::readBankByte (std::uint8_t& value)
{
	std::size_t offset = bankPosition;
	for (const auto& segment : pcmBank)
	{
		if (offset < segment.size)
		{
			value = segment.data[offset];
			++bankPosition;
			return true;
		}

		offset -= segment.size;
	}

	return false;
}

bool VgmCommandReader::next (VgmCommand& command)
{
	if (hasResume)
	{
		command = resume;
		hasResume = false;
		return true;
	}

	if (! isValid())
	{
		return false;
	}

	const std::uint8_t cmd = data[position];
	const int length = commandLength (cmd, data + position, end - position);
	if (length <= 0 || position + (std::size_t) length > end)
	{
		return false;
	}

	command = VgmCommand {};
	command.command = cmd;
	command.sampleTime = sampleTime;
	command.kind = VgmCommand::Kind::ignored;

	switch (cmd)
	{
	case 0x61:
		sampleTime += readLe16 (data + position + 1);
		command.kind = VgmCommand::Kind::wait;
		break;
	case 0x62:
		sampleTime += 735;
		command.kind = VgmCommand::Kind::wait;
		break;
	case 0x63:
		sampleTime += 882;
		command.kind = VgmCommand::Kind::wait;
		break;
	case 0x66:
		command.kind = VgmCommand::Kind::end;
		position = end;
		return true;
	case 0x67:
	{
		// Data block. Every block is remembered with its payload (a caller may need a chip's ROM);
		// bank 0 is also kept as the YM2612's PCM bank, which is what 0x8n reads its sample bytes
		// from. Bank 0 is type 0x00, and type 0x80 lands in the same bank in libvgm - the bit that
		// differs only says the data came from a ROM, and such a block spends its first eight
		// payload bytes on the ROM size and offset, so the samples start after them.
		const std::uint8_t type = data[position + 2];
		const std::uint32_t blockSize = readLe32 (data + position + 3) & 0x7FFFFFFFu;
		const bool isRom = (type & 0x80) != 0;
		const std::size_t headerSize = isRom ? 8 : 0;
		const std::size_t payloadSize = (blockSize > headerSize) ? (std::size_t) (blockSize - headerSize) : 0;

		VgmDataBlock block;
		block.type = type;
		block.romSize = isRom ? readLe32 (data + position + 7) : 0;
		block.romOffset = isRom ? readLe32 (data + position + 11) : 0;
		block.data = data + position + 7 + headerSize;
		block.size = payloadSize;
		blocks.push_back (block);

		if ((type & 0x7Fu) == 0x00)
		{
			pcmBank.push_back ({ block.data, block.size });
		}
		break;
	}
	case 0xE0:
		// "Set YM2612 PCM data offset": the stream skips to a position in the bank it built.
		bankPosition = readLe32 (data + position + 1);
		break;
	default:
		if (cmd >= 0x70 && cmd <= 0x7F)
		{
			sampleTime += (std::uint32_t) (cmd & 0x0F) + 1;
			command.kind = VgmCommand::Kind::wait;
		}
		else if (cmd >= 0x80 && cmd <= 0x8F)
		{
			// YM2612 DAC stream: write the next byte of the PCM bank to port 0 register 0x2A, then
			// wait the low nibble. The write is what the DAC channel's notes are made of, and the
			// wait is the *only* thing carrying time in a DAC stream - it used to be dropped here,
			// which shortened every Mega Drive song by all of it (Green Hill Zone: 31.5 s of 52.9 s)
			// and made the MIDI run ahead of the audio until the sequence ended.
			std::uint8_t sample = 0;
			const std::uint32_t wait = (std::uint32_t) (cmd & 0x0F);
			if (! readBankByte (sample))
			{
				sampleTime += wait;
				command.kind = VgmCommand::Kind::wait;
				break;
			}

			command.command = 0x52;		// the format's own command for a YM2612 port 0 write
			command.kind = VgmCommand::Kind::registerWrite;
			command.addr = 0x2A;		// the DAC sample register
			command.data = sample;
			sampleTime += wait;

			resume = VgmCommand {};
			resume.command = cmd;
			resume.kind = VgmCommand::Kind::wait;
			resume.sampleTime = sampleTime;
			hasResume = true;
		}
		else if (cmd == 0x50)
		{
			// SN76489 takes a single byte: the latch bit and the payload share it
			command.kind = VgmCommand::Kind::registerWrite;
			command.addr = data[position + 1];
			command.data = data[position + 1];
		}
		else if (cmd == 0xC8)
		{
			// X1-010: the register offset is 16-bit and big-endian (libvgm's Cmd_Ofs16_Data8), so
			// the high byte travels in `port` - the only free byte on the command - and the decoder
			// puts the address back together. Offsets above 0x7F are envelope and wavetable data.
			const std::uint16_t offset = (std::uint16_t) (((std::uint16_t) data[position + 1] << 8)
														  | (std::uint16_t) data[position + 2]);
			command.kind = VgmCommand::Kind::registerWrite;
			command.port = (std::uint8_t) ((offset >> 8) & 0x7F);
			command.addr = (std::uint8_t) (offset & 0xFF);
			command.data = data[position + 3];
		}
		else if (cmd == 0xD0)
		{
			// YMF278B (OPL4): the address byte carries the chip in bit 7 and the port in the
			// low seven bits. Ports 0 and 1 are the two OPL3 banks, 2..5 the wavetable half.
			command.kind = VgmCommand::Kind::registerWrite;
			command.port = data[position + 1];
			command.addr = data[position + 2];
			command.data = data[position + 3];
		}
		else if (cmd == 0xD4)
		{
			// C140: the same 16-bit big-endian offset as the X1-010, and the same split - bit 7 of
			// the first byte picks the chip, the rest is the high byte of the address. The voices
			// are the low 0x180 of it, so the decoder needs both halves.
			const std::uint16_t offset = (std::uint16_t) (((std::uint16_t) data[position + 1] << 8)
														  | (std::uint16_t) data[position + 2]);
			command.kind = VgmCommand::Kind::registerWrite;
			command.port = (std::uint8_t) ((offset >> 8) & 0xFF);
			command.addr = (std::uint8_t) (offset & 0xFF);
			command.data = data[position + 3];
		}
		else if (cmd == 0xE1)
		{
			// C352: a 16-bit big-endian offset and a 16-bit big-endian value - the only command
			// where the register write is a whole word. Bit 15 of the offset picks the second chip
			// and the rest is the offset, so the high byte travels in `port` as the C140's does;
			// the value's two halves go in `data` (high) and `data2` (low).
			const std::uint16_t offset = (std::uint16_t) (((std::uint16_t) data[position + 1] << 8)
														  | (std::uint16_t) data[position + 2]);
			command.kind = VgmCommand::Kind::registerWrite;
			command.port = (std::uint8_t) ((offset >> 8) & 0xFF);
			command.addr = (std::uint8_t) (offset & 0xFF);
			command.data = data[position + 3];
			command.data2 = data[position + 4];
		}
		else if (cmd == 0xD1)
		{
			// YMF271 (OPX): the port byte carries the chip in bit 7 and the bank in the low seven
			// bits - 0-3 are the FM banks, 4 the PCM registers, 6 the timer/sync registers. The
			// decoder needs the bank, because a write to bank 0 is what copies the synced
			// registers into the other banks of a four-operator group.
			command.kind = VgmCommand::Kind::registerWrite;
			command.port = data[position + 1];
			command.addr = data[position + 2];
			command.data = data[position + 3];
		}
		else if (cmd == 0xD2)
		{
			// K051649 (SCC): the command carries both bytes of one SCC access, and libvgm hands
			// the chip two writes - the latch at register "a" and the value at "a + 1". Mirroring
			// that here keeps the decoded notes aligned with what the audio engine plays.
			const std::uint8_t addrByte = data[position + 1];
			command.kind = VgmCommand::Kind::registerWritePair;
			command.port = (std::uint8_t) (addrByte >> 7);
			command.addr = (std::uint8_t) ((addrByte & 0x7F) << 1);
			command.data = data[position + 2];
			command.data2 = data[position + 3];
		}
		else if (length == 3 && ((cmd >= 0x50 && cmd <= 0x5F) || (cmd >= 0xA0 && cmd <= 0xBF)))
		{
			const std::uint8_t addrByte = data[position + 1];
			command.kind = VgmCommand::Kind::registerWrite;
			command.data = data[position + 2];
			if (splitsAddressByte (cmd))
			{
				command.port = (std::uint8_t) (addrByte >> 7);
				command.addr = registerAddress (cmd, (std::uint8_t) (addrByte & 0x7F));
			}
			else
			{
				command.addr = addrByte;
			}
		}
		break;
	}

	position += (std::size_t) length;
	return true;
}

} // namespace toybox::backends::libvgm
