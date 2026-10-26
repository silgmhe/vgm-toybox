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

/** One channel's waveform, kept from one frame to the next.

	A channel whose snapshot carries no cycle of its own has its shape derived from three register
	values and the size of the window - and those change when the chip is programmed, not thirty
	times a second. The panel used to rebuild it every frame anyway: a vector copy, a fresh
	juce::Path and a stroke, for a line that had not moved.

	One of these per channel, owned by whoever paints. A channel that *does* carry a cycle (the
	snapshot's own, which changes as the core plays) is drawn as given and never cached here.
*/
struct WavePathCache
{
	juce::Path path;
	int waveKind = -1;
	float duty = -1.0f;
	float shape = 0.0f;
	int width = 0;
	int height = 0;

	/** True when `path` holds something worth stroking. Parameters already recorded either way. */
	bool valid = false;
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
					  const KeyPainter& paintKeys,
					  WavePathCache& waveCache);

/** Mute and solo, drawn with the mixer's own face and colours: mute fills red, solo amber, and a
	toggled one carries the matching glow. Anything else would make the two views look like
	different applications. */
void paintToggle (juce::Graphics& g, juce::Rectangle<int> area,
				  const char* letter, bool on, bool mute, bool hovered);

/** The waveform column: a rounded window over the card, a hairline, and the cycle inside it. */
void paintWave (juce::Graphics& g, juce::Rectangle<int> area,
				const playback::ChannelState& channel, WavePathCache& cache);

/** The note's velocity, not its loudness - see the comment on the implementation. */
void paintMeter (juce::Graphics& g, juce::Rectangle<int> area,
				 const playback::ChannelState& channel);

} // namespace toybox::ui::keyboard
