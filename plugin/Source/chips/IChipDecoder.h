#pragma once

#include <cstdint>

#include "ChipTypes.h"

namespace toybox::chips
{

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
	virtual void advance (std::uint32_t sampleTime) = 0;

	virtual int channelCount() const = 0;

	/** The channel's current state; index out of range returns a default (present = false). */
	virtual ChannelState channelState (int channel) const = 0;

	/** The note table for one channel - per channel, because a chip's noise channel and its
		tone channels do not share a pitch mapping. */
	virtual const NoteTable& noteTable (int channel) const = 0;

	virtual ChannelKind channelKind (int channel) const = 0;

	/** True when the channel should be treated as a drum track (GM drum key mapping). */
	virtual bool isDrum (int channel) const { return false; }
};

} // namespace toybox::chips
