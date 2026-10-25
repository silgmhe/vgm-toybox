#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace toybox::ui
{

/** The "repeat" glyph (a ring with an arrow head on the top and the bottom edge).

	Shared between IconButton (the loop toggle), the loop point marker above the position
	slider and the drop hint area. */
void drawRepeatGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour);

/** The "play from here" glyph (a bar followed by a right-pointing triangle) of the playback
	start cursor above the position slider. */
void drawPlayStartGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour);

/** The "mixer" glyph (three vertical faders with their knobs at different heights) of the
	mixer card's on/off toggle. */
void drawSlidersGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour);

/** The "spread outputs" glyph: one bus on the left fanning out into several output buses. */
void drawSpreadOutputsGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour);

/** The "send to master" glyph: several tracks on the left converging into one bus on the right. */
void drawMergeToMasterGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour);

/** The "remove DC" glyph: a flat line that curves up and levels off - the shape of a high pass,
	which is what the button switches on. */
void drawDcBlockGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour);

/** The "linked to the host" glyph: two interlocking chain links, of the DAW sync toggle. */
void drawSyncGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour);

/** The "cue marker" glyph: a flag on a pole - the cue point the song starts at. */
void drawCueGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour);

/** The "melodic channel" glyph: a small piano keyboard (four white keys, two black ones).

	Used next to MIDI channels whose notes carry the pitch the chip plays, as opposed to the
	sample glyph below. */
void drawMelodyGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour);

/** The "one-shot" glyph: a sample waveform - one attack, then a decaying burst.

	Used next to MIDI channels that only send a fixed trigger note (ADPCM, DAC, noise): the
	shape says "a sample is playing", not "a pitch is playing". */
void drawSampleGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour);

/** The two above, stacked: a note that belongs to a fixed set of them. */
void drawDiscreteGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour);

/** The MIDI mark: the DIN ring, its five contacts and the alignment notch. */
void drawMidiGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour);

/** The MIDI mark as an image, for popup menu items (juce::PopupMenu takes an image, not a
	Graphics call). `pixels` is the raster size: pass four times the drawn size, the menu scales
	it into its icon column. */
juce::Image midiGlyphImage (int pixels, juce::Colour colour);

} // namespace toybox::ui
