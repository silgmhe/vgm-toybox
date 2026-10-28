#include "KeyboardRowPaint.h"

#include "ButtonPaint.h"
#include "KeyboardLayout.h"
#include "i18n/LanguageManager.h"
#include "SoftShadow.h"
#include "Theme.h"
#include "playback/WaveShape.h"

namespace toybox::ui::keyboard
{

namespace
{
	/** The window the waveform is drawn in: a shade off the card, a hairline, and a soft shading
		along the top edge. Enough to read as something seen through glass, and cheap - nothing is
		blurred, because the surface behind it is a flat colour. */
	constexpr float kWindowRadius = 4.0f;
	constexpr float kWindowShadeDepth = 7.0f;
	constexpr float kWindowShadeAlpha = 0.12f;

	/** Inside that window the cycle is inset so the line does not touch the frame. */
	constexpr int kCycleInsetX = 4;
	constexpr int kCycleInsetY = 3;

	/** A level under this is silence rather than a quiet note. */
	constexpr float kSilenceLevel = 0.001f;

	/** The row's card: the material the mixer strips use, with the same gaussian shadow. */
	constexpr int kTileShadowBlur = 6;
	constexpr int kTileShadowOffsetY = 2;
	constexpr float kTileTintAlpha = 0.32f;
	constexpr float kTileBorderAlpha = 0.6f;
}

void paintGroupHeader (juce::Graphics& g, const Row& row, int channelCount)
{
	const int count = juce::jlimit (0, 32, channelCount);

	const auto area = row.area;

	g.setColour (theme::secondaryText ().withAlpha (0.28f));
	g.fillRoundedRectangle (area.toFloat (), theme::buttonRadius);

	g.setColour (theme::primaryText ());
	g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
	g.drawText (row.title, area.reduced (10, 0), juce::Justification::centredLeft, false);

	g.setColour (theme::secondaryText ());
	g.setFont (juce::Font (juce::FontOptions (11.0f)));
	g.drawText (juce::String (count) + " ch", area.reduced (10, 0),
				juce::Justification::centredRight, false);
}

void paintChannelRow (juce::Graphics& g,
					  const Row& row,
					  const playback::ChannelState& channel,
					  bool muted,
					  bool soloed,
					  int octaveShift,
					  RowHover hover,
					  const KeyPainter& paintKeys,
					  RowPaintCache& cache)
{
	const bool audible = channel.active && channel.volume > kSilenceLevel;

	// A tile per row, with its shadow drawn through the same helper the cards use - a real
	// gaussian blur, not a second rectangle pretending to be one. The shadow stays inside the
	// row's own height, so nothing gets cut off.
	//
	// Through the cache, though: this row is repainted thirty times a second and its shadow is a
	// function of its rectangle and the palette, never of the audio. Twenty-six real blurs per
	// frame is what the GPU report was about.
	const auto tile = row.area.reduced (0, 2).toFloat ();

	drawCachedShadow (g, cache.tileShadow, tile, theme::buttonRadius, theme::cardKeyShadow (),
					  kTileShadowBlur, juce::Point<int> (0, kTileShadowOffsetY));

	g.setColour (theme::track ().withAlpha (kTileTintAlpha));
	g.fillRoundedRectangle (tile, theme::buttonRadius);
	g.setColour (theme::cardBorder ().withAlpha (kTileBorderAlpha));
	g.drawRoundedRectangle (tile.reduced (0.5f), theme::buttonRadius, 1.0f);

	g.setColour (audible ? theme::primaryText () : theme::secondaryText ());
	g.setFont (juce::Font (juce::FontOptions (12.0f)));
	g.drawText (juce::String (channel.name), row.name, juce::Justification::centredLeft, false);

	paintToggle (g, row.mute, "M", muted, true, hover.mute);
	paintToggle (g, row.solo, "S", soloed, false, hover.solo);

	// The note, or a dash when the channel has no pitch to speak of.
	if (channel.analysing)
	{
		// The row is playing and its shape is drawn from the registers, but its note is a sample
		// slot and a slot means nothing until the walk over the song has numbered them. Saying so
		// is better than a note that changes under the reader a second later.
		g.setColour (theme::secondaryText ());
		g.setFont (juce::Font (juce::FontOptions (11.0f)));
		g.drawText (i18n::tr ("visualiser.analysing"), row.note, juce::Justification::centred, false);
	}
	else
	{
		g.setColour (channel.note >= 0 ? theme::primaryText () : theme::secondaryText ());
		g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
		g.drawText (midiNoteName (channel.note, octaveShift), row.note,
					juce::Justification::centred, false);
	}

	paintWave (g, row.wave, channel, cache.wave);

	if (paintKeys != nullptr)
	{
		paintKeys (g, row.keys, channel.note);
	}

	paintMeter (g, row.meter, channel);
}

void paintToggle (juce::Graphics& g, juce::Rectangle<int> area,
				  const char* letter, bool on, bool mute, bool hovered)
{
	if (on)
	{
		drawButtonGlow (g, area, mute ? theme::muteGlow () : theme::soloGlow (),
						theme::compactGlowBlur);
	}

	ButtonFace face;
	face.level = on ? ButtonLevel::filled : ButtonLevel::material;
	face.toggled = on;
	face.hovered = hovered;
	face.fill = mute ? theme::mute () : theme::solo ();

	drawButtonFace (g, area.toFloat (), face);

	g.setColour (on ? (mute ? theme::onAccent () : theme::primaryText ()) : theme::secondaryText ());
	g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
	g.drawText (letter, area, juce::Justification::centred, false);
}

namespace
{
	/** Builds one cycle's line in coordinates local to its window - origin at the top left, no
		knowledge of where the window is on screen.

		That distinction is the whole point. When this took a rectangle it wrote absolute positions
		into the path, and the cache key was the window's size - so scrolling the list moved the
		window, the size stayed the same, the cache was still thought good, and the line was stroked
		at the row's previous height. Two lines per row, one of them forty pixels below the other.
	*/
	void buildCyclePath (juce::Path& path, int width, int height, const std::vector<float>& cycle)
	{
		path.clear ();

		const auto count = (int) cycle.size ();
		const float half = (float) height * 0.5f - 2.0f;
		const float span = (float) width / (float) count;

		for (int i = 0; i < count; ++i)
		{
			const float level = juce::jlimit (-1.0f, 1.0f, cycle[(std::size_t) i]);
			const float y = (float) height * 0.5f - level * half;
			const float x0 = span * (float) i;

			if (i == 0)
			{
				path.startNewSubPath (x0, y);
			}
			else
			{
				path.lineTo (x0, y);
			}

			path.lineTo (x0 + span, y);
		}
	}

	/** Strokes a cycle built by buildCyclePath() where `area` says it belongs. */
	void strokeCycle (juce::Graphics& g, const juce::Path& path, juce::Rectangle<int> area)
	{
		juce::Graphics::ScopedSaveState saved (g);
		g.addTransform (juce::AffineTransform::translation ((float) area.getX (), (float) area.getY ()));
		g.strokePath (path, juce::PathStrokeType (1.0f));
	}

	void paintNoCycle (juce::Graphics& g, juce::Rectangle<int> area)
	{
		g.setColour (theme::secondaryText ());
		g.setFont (juce::Font (juce::FontOptions (10.5f)));
		g.drawText ("-", area, juce::Justification::centred, false);
	}
}

void paintWave (juce::Graphics& g, juce::Rectangle<int> area,
				const playback::ChannelState& channel, WavePathCache& cache)
{
	const auto window = area.toFloat ();

	g.setColour (theme::card ().darker (0.05f));
	g.fillRoundedRectangle (window, kWindowRadius);

	{
		juce::Graphics::ScopedSaveState clip (g);
		g.reduceClipRegion (window.toNearestInt ());

		juce::ColourGradient shade (theme::shadowTint ().withAlpha (kWindowShadeAlpha),
									window.getX (), window.getY (),
									juce::Colours::transparentBlack,
									window.getX (), window.getY () + kWindowShadeDepth, false);
		g.setGradientFill (shade);
		g.fillRoundedRectangle (window, kWindowRadius);
	}

	g.setColour (theme::cardBorder ().withAlpha (0.7f));
	g.drawRoundedRectangle (window.reduced (0.5f), kWindowRadius, 1.0f);

	area = area.reduced (kCycleInsetX, kCycleInsetY);

	// A cycle the snapshot carries is the core's own and changes as it plays, so it is drawn as
	// given. The one derived from the registers is a function of three values and the window's
	// size - and it was being rebuilt thirty times a second anyway: a vector copy, a fresh
	// juce::Path and a stroke, for a line that had not moved.
	if (! channel.wave.empty ())
	{
		if (channel.wave.size () < 2)
		{
			paintNoCycle (g, area);
			return;
		}

		juce::Path path;
		buildCyclePath (path, area.getWidth (), area.getHeight (), channel.wave);

		g.setColour (theme::accent ().withAlpha (channel.active ? 0.9f : 0.35f));
		strokeCycle (g, path, area);
		return;
	}

	// waveKind is the sentinel: -1 means nothing has been built for this row yet, which is not the
	// same as "built, and there was nothing to build" - that case has valid == false with the
	// parameters recorded, and only needs the dash repainted.
	const bool stale = cache.waveKind < 0
					|| cache.waveKind != channel.waveKind
					|| cache.dutyStep != dutySteps (channel.duty)
					|| cache.shapeParameter != channel.shapeParameter
					|| cache.width != area.getWidth ()
					|| cache.height != area.getHeight ();

	if (stale)
	{
		const auto cycle = playback::waveform::cycleFor (channel.waveKind, channel.duty, channel.shapeParameter);

		cache.valid = false;

		if (cycle.size () >= 2)
		{
			buildCyclePath (cache.path, area.getWidth (), area.getHeight (), cycle);
			cache.valid = true;
		}
		else
		{
			cache.path.clear ();
		}

		cache.waveKind = channel.waveKind;
		cache.dutyStep = dutySteps (channel.duty);
		cache.shapeParameter = channel.shapeParameter;
		cache.width = area.getWidth ();
		cache.height = area.getHeight ();
	}

	if (! cache.valid)
	{
		paintNoCycle (g, area);
		return;
	}

	g.setColour (theme::accent ().withAlpha (channel.active ? 0.9f : 0.35f));
	strokeCycle (g, cache.path, area);
}

void paintMeter (juce::Graphics& g, juce::Rectangle<int> area,
				 const playback::ChannelState& channel)
{
	// Two pixels, not eight: the meter's rect is twenty tall, and reducing it by eight on each
	// side left a zero-height strip that drew nothing at all.
	auto track = area.reduced (0, 2);

	g.setColour (theme::track ());
	g.fillRoundedRectangle (track.toFloat (), 2.0f);

	// The note's own level - velocity, not loudness. This is a different quantity from the
	// mixer's meters, which show what is coming out of the speakers and turn amber and red as it
	// approaches full scale. A pulse played at full volume is the normal case here, not a
	// warning, so it gets the accent colour and no threshold.
	const float level = juce::jlimit (0.0f, 1.0f, channel.volume);

	if (level > kSilenceLevel)
	{
		g.setColour (theme::accent ().withAlpha (0.9f));
		g.fillRoundedRectangle (track.toFloat ().withWidth ((float) track.getWidth () * level), 2.0f);
	}
}

} // namespace toybox::ui::keyboard
