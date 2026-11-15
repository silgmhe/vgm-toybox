#pragma once

#include <optional>

#include <juce_graphics/juce_graphics.h>

#include "chips/ChipTypes.h"
#include "playback/ChipState.h"

namespace toybox::ui
{

/**
	**What a channel makes its sound with** - the seven kinds a reader can tell apart at the size
	these are drawn at.

	Deliberately not `chips::ChannelKind` (41 values: honest, unreadable) and not
	`waveform::Kind` (which merges PCM samples and wavetables into `stored`, because for *drawing a
	wave* the difference does not matter). Here it does: a C140 drum and an SCC wavetable are the
	two things a reader most wants to distinguish, and the channel names alone do not say which is
	which - `Square A` next to `Pulse 1` next to `Ch 3`.

	`delta` (the NES DMC) is not a value of its own: it is a 1-bit sampled channel and reads as
	PCM. `fmDrum` / `adpcmDrum` likewise ride with `fm` / `pcm` - the channel name says "drum".
*/
enum class ChannelGlyph
{
	pulse,
	triangle,
	saw,
	noise,
	fm,
	wavetable,
	pcm
};

/** **The one place the 41 real channel kinds collapse into the seven marks.**

	Not `waveform::Kind`, and deliberately a mapping of its own: that one exists to decide *what
	wave to draw*, so it merges PCM samples and wavetables into `stored` (both are read out of
	memory, and a waveform looks the same either way). Here they must not merge - "this is a drum
	sample" and "this is a wavetable" are the two things a reader most wants told apart, and the
	names do not say which is which.

	`delta` (the NES DMC) lands on `pcm`: it is a 1-bit sampled channel. `fmDrum` / `adpcmDrum` ride
	with `fm` / `pcm` because the channel name already says "drum". Exhaustive on purpose - no
	`default`, so adding a kind to the enum fails to compile here rather than drawing nothing.
*/
inline ChannelGlyph channelGlyphFor (chips::ChannelKind kind)
{
	switch (kind)
	{
	case chips::ChannelKind::nesPulse:
	case chips::ChannelKind::gbPulse:
	case chips::ChannelKind::vrc6Pulse:
	case chips::ChannelKind::mmc5Pulse:
	case chips::ChannelKind::aySquare:
	case chips::ChannelKind::ayEnvelope:
	case chips::ChannelKind::sn76489Square:
	case chips::ChannelKind::saaSquare:
	case chips::ChannelKind::pokeySquare:
		return ChannelGlyph::pulse;

	case chips::ChannelKind::nesTriangle:
		return ChannelGlyph::triangle;

	case chips::ChannelKind::vrc6Saw:
		return ChannelGlyph::saw;

	case chips::ChannelKind::nesNoise:
	case chips::ChannelKind::gbNoise:
	case chips::ChannelKind::ayNoise:
	case chips::ChannelKind::sn76489Noise:
	case chips::ChannelKind::wsNoise:
	case chips::ChannelKind::c6280Noise:
		return ChannelGlyph::noise;

	case chips::ChannelKind::vrc7Fm:
	case chips::ChannelKind::fm:
	case chips::ChannelKind::fmDrum:
		return ChannelGlyph::fm;

	// The wavetables: one cycle, read from memory, scanned.
	case chips::ChannelKind::gbWave:
	case chips::ChannelKind::fdsWave:
	case chips::ChannelKind::n163Wave:
	case chips::ChannelKind::c6280Wave:
	case chips::ChannelKind::sccWave:
	case chips::ChannelKind::x1Wave:
	case chips::ChannelKind::wavetableWave:
	case chips::ChannelKind::wsWave:
		return ChannelGlyph::wavetable;

	// The samplers, and the DMC: recorded (or delta-coded) material, not a synthesised cycle.
	case chips::ChannelKind::nesDmc:
	case chips::ChannelKind::rf5cPcm:
	case chips::ChannelKind::c140Pcm:
	case chips::ChannelKind::c352Pcm:
	case chips::ChannelKind::k053260Pcm:
	case chips::ChannelKind::k054539Pcm:
	case chips::ChannelKind::qsoundPcm:
	case chips::ChannelKind::ga20Pcm:
	case chips::ChannelKind::okimAdpcm:
	case chips::ChannelKind::segaPcm:
	case chips::ChannelKind::opnAdpcmB:
	case chips::ChannelKind::adpcmDrum:
		return ChannelGlyph::pcm;

	case chips::ChannelKind::unknown:
		break;
	}

	return ChannelGlyph::pulse;		// unreachable; the switch is exhaustive
}

/** The same seven, read from the **per-frame channel state** instead of the static kind.

	The keyboard rows are built from `ChannelState`, which carries the eight-value `waveKind` rather
	than the 41-value `ChannelKind` - one value short of what the marks need, because `stored` covers
	both. `tableWave` closes that gap: it says whether `wave` is one cycle of a table read out of
	memory or a recording being played back, which is exactly the wavetable/sample difference this
	enum exists to show. Without it a C140 drum drew as a wavetable.

	`nullopt` when the channel generates nothing drawable (`waveKind == 0`).
*/
inline std::optional<ChannelGlyph> channelGlyphFor (const playback::ChannelState& state)
{
	switch (state.waveKind)
	{
	case 1:		// stored: a table, or a recording
		return state.tableWave ? ChannelGlyph::wavetable : ChannelGlyph::pcm;
	case 2:  return ChannelGlyph::pulse;
	case 3:  return ChannelGlyph::triangle;
	case 4:  return ChannelGlyph::noise;
	case 5:  return ChannelGlyph::pcm;		// delta: the NES DMC counts as sampled
	case 6:  return ChannelGlyph::saw;
	case 7:  return ChannelGlyph::fm;
	case 0:  break;
	}

	return std::nullopt;
}

/** Draws `glyph` inside `area`, stroked in `colour`.

	Implemented in `ChannelGlyphs.generated.cpp`, which `plugin/tools/svg_to_channel_glyphs.py`
	produces from `plugin/assets/icon/*.svg`. The SVG is the design; that file is the drawing.
*/
void drawChannelGlyph (juce::Graphics& g, ChannelGlyph glyph,
					   juce::Rectangle<float> area, juce::Colour colour);

} // namespace toybox::ui
