#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ChipTypes.h"

namespace toybox::chips
{

/** What a decoder needs besides its register writes.

	The assembler used to hold this knowledge one hand-written line per chip: the block type that
	carries its samples, how large the image may be, whether it is a ROM or a RAM upload, and which
	clock to assume when the song names none. Those are facts about the chip, so the chip states
	them and the assembler only serves them - the same rule that moved DEVID_* out of the chip layer.

	Declared as `static constexpr` per decoder (`C140Decoder::kRequirements`), because nothing about
	it changes per song. */
struct ChipRequirements
{
	uint32_t fallbackClock = 0;	// used when the device reports no clock of its own
	bool hasMemory = false;				// the decoder needs a ROM or RAM image
	uint8_t memoryType = 0;		// the stream's block type that carries it
	std::size_t memoryBytes = 0;		// the chip's addressable limit
	bool memoryIsRam = false;			// RAM uploads concatenate; ROM blocks place at an offset
	bool clipToContent = false;			// mask by what was sent rather than by the declared size
};

/** The image the assembler built for a decoder, plus the board variant it was found in.

	`bytes` is borrowed: it lives for the duration of one flush and the decoder copies what it needs.
	`banking` only means anything to the C140, whose ROM offset folds a voice's bank into it. */
struct ChipMemory
{
	const std::vector<uint8_t>* bytes = nullptr;
	uint8_t banking = 0;
};

/** A chip's register-level model.

	A decoder only answers "what does this channel sound like right now"; it never decides what
	a MIDI note is. That split is what lets the note extractor stay chip-agnostic and lets a new
	chip be added without touching the encoder.

	Call order per device: reset(), then writes in stream order interleaved with advance(). The
	decoder must be usable with a clock it does not recognise: fall back to a sensible default
	rather than failing, because a missing clock in the header is common. */
class IChipDecoder
{
public:
	virtual ~IChipDecoder() = default;

	/** @param config the chip's clock and the unit its write timestamps are expressed in. A missing
		clock (0) is common and must fall back to a default rather than fail. */
	virtual void reset (const ChipConfig& config) = 0;

	/** One register write, positioned at an absolute sample time. Decoders may apply it
		immediately or queue it for the next advance(), depending on the chip. */
	virtual void write (const RegisterWrite& write) = 0;

	/** Move the internal timers/sequencers forward to this absolute sample time. */
	virtual void advance (uint32_t sampleTime) = 0;

	virtual int channelCount() const = 0;

	/** The channel's current state; index out of range returns a default (present = false). */
	virtual ChannelState channelState (int channel) const = 0;

	/** The note table for one channel - per channel, because a chip's noise channel and its
		tone channels do not share a pitch mapping. */
	virtual const NoteTable& noteTable (int channel) const = 0;

	virtual ChannelKind channelKind (int channel) const = 0;

	/** True when the channel should be treated as a drum track (GM drum key mapping). */
	virtual bool isDrum (int channel) const { return false; }

	/** Hand over the image `kRequirements` asked for. Called once, before the first write.
		The default ignores it, which is right for every chip that names a pitch from registers. */
	virtual void attachMemory (const ChipMemory&) {}
};

} // namespace toybox::chips
