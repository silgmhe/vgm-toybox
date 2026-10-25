#pragma once

#include "KeyboardRowLayout.h"

#include "playback/ChipState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace toybox::ui::keyboard
{

/** Draws the keys column for one row.

	The keyboard is the one part of a row the panel keeps: it owns the visible span, the scroll
	position and the pressed key, none of which a painter should have to know about. Everything
	else here is a pure function of the row's rectangle and the channel's state.
*/
using KeyPainter = std::function<void (juce::Graphics&, juce::Rectangle<int>, int note)>;

/** A chip's title, with how many channels it has. `channelCount` is the caller's, because the
	rows no longer carry the snapshot. */
void paintGroupHeader (juce::Graphics& g, const Row& row, int channelCount);

/** Which of a row's two buttons the pointer is over, if any. */
struct RowHover
{
	bool mute = false;
	bool solo = false;
};

/** One channel: its card, its name, its mute and solo, its note, its waveform, its keys and its
	meter. The keys come from `paintKeys`; everything else is drawn here. */
void paintChannelRow (juce::Graphics& g,
					  const Row& row,
					  const playback::ChannelState& channel,
					  bool muted,
					  bool soloed,
					  int octaveShift,
					  RowHover hover,
					  const KeyPainter& paintKeys);

/** Mute and solo, drawn with the mixer's own face and colours: mute fills red, solo amber, and a
	toggled one carries the matching glow. Anything else would make the two views look like
	different applications. */
void paintToggle (juce::Graphics& g, juce::Rectangle<int> area,
				  const char* letter, bool on, bool mute, bool hovered);

/** The waveform column: a rounded window over the card, a hairline, and the cycle inside it. */
void paintWave (juce::Graphics& g, juce::Rectangle<int> area,
				const playback::ChannelState& channel);

/** The note's velocity, not its loudness - see the comment on the implementation. */
void paintMeter (juce::Graphics& g, juce::Rectangle<int> area,
				 const playback::ChannelState& channel);

} // namespace toybox::ui::keyboard
