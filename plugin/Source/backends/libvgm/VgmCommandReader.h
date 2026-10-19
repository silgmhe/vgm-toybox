#pragma once

#include <cstddef>
#include <cstdint>

namespace toybox::backends::libvgm
{

/** One command of a VGM command stream, already aligned and stripped of its length.

	The reader owns the format knowledge (header offsets, command lengths, wait commands) so
	that the MIDI builder only ever sees "this chip register was written at this sample". The
	device mapping is deliberately *not* part of this struct: which libvgm device answers a
	command is the caller's decision, so a stream can be replayed against any device layout. */
struct VgmCommand
{
	enum class Kind
	{
		registerWrite,	// addr/data carry the payload, port the second-chip flag
		wait,			// sampleTime has already been advanced
		end,			// 0x66 - the caller should stop
		ignored			// data block, seek, or an unknown command
	};

	Kind kind = Kind::ignored;
	std::uint8_t command = 0;		// the raw VGM command byte
	std::uint8_t port = 0;			// 0 / 1 - the second chip of a pair (bit 7 of the address byte)
	std::uint8_t addr = 0;			// register / index byte
	std::uint8_t data = 0;			// value byte
	std::uint32_t sampleTime = 0;	// absolute position of the command, in 44100 Hz samples
};

/** Walks one uncompressed VGM image. The stream ends at the EOF offset, the GD3 tag or the
	0x66 command, whichever comes first - the same boundaries libvgm uses. */
class VgmCommandReader
{
public:
	VgmCommandReader (const void* data, std::size_t size);

	/** True while the stream is valid; next() is only meaningful while valid. */
	bool isValid (void) const { return data != nullptr && position < end; }

	/** Advances past one command. Returns false at the end of the stream. */
	bool next (VgmCommand& command);

private:
	bool readHeader (void);

	const std::uint8_t* data = nullptr;
	std::size_t size = 0;
	std::size_t position = 0;
	std::size_t end = 0;
	std::uint32_t sampleTime = 0;
};

} // namespace toybox::backends::libvgm
