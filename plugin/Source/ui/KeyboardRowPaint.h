#pragma once

#include "KeyboardRowLayout.h"
#include "SoftShadow.h"

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

	// The same steps the repaint decision compares, from the same functions (keyboard::dutySteps).
	// They used to be the raw floats here and the steps there, which meant the two caches had
	// different ideas of what "the duty changed" means: a change too small to repaint the column
	// could still have rebuilt the path, and one that repainted it could have reused a path the
	// finer comparison thought was still current.
	int waveKind = -1;
	int dutyStep = -1;
	int shapeParameter = -1;

	int width = 0;
	int height = 0;

	/** True when `path` holds something worth stroking. Parameters already recorded either way. */
	bool valid = false;
};

/** Everything one row's painter carries from one frame to the next.

	Both members exist for the same reason and it is the one written up in
	docs/issue/2026-10-26-keyboard-view-gpu-cost.md: the view repaints many times a second, and
	almost none of what it paints has changed since the last time. The waveform is a function of
	three register values; the row's card shadow is a function of its rectangle and the palette.
	Neither is a function of the clock - which is why the rate being a setting now costs nothing.
*/
struct RowPaintCache
{
	WavePathCache wave;
	ShadowImage tileShadow;

	/** The keyboard bed's shadow. Per row, not shared: the bed is as wide as the keys that fit the
		column, and two rows can differ by a whole key when the columns are not all the same width.
		A single image would then be rebuilt for every row, every frame - the cost this is here to
		remove.
	*/
	ShadowImage bedShadow;
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
					  RowPaintCache& cache);

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
