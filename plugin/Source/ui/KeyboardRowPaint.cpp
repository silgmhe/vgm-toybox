#include "KeyboardRowPaint.h"

#include "ButtonPaint.h"
#include "KeyboardLayout.h"
#include "SoftShadow.h"
#include "Theme.h"
#include "WaveShape.h"

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
					  const KeyPainter& paintKeys)
{
	const bool audible = channel.active && channel.volume > kSilenceLevel;

	// A tile per row, with its shadow drawn through the same helper the cards use - a real
	// gaussian blur, not a second rectangle pretending to be one. The shadow stays inside the
	// row's own height, so nothing gets cut off.
	const auto tile = row.area.reduced (0, 2).toFloat ();

	drawSoftShadow (g, tile, theme::buttonRadius, theme::cardKeyShadow (), kTileShadowBlur,
					juce::Point<int> (0, kTileShadowOffsetY));

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
	g.setColour (channel.note >= 0 ? theme::primaryText () : theme::secondaryText ());
	g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
	g.drawText (midiNoteName (channel.note, octaveShift), row.note,
				juce::Justification::centred, false);

	paintWave (g, row.wave, channel);

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

void paintWave (juce::Graphics& g, juce::Rectangle<int> area,
				const playback::ChannelState& channel)
{
	const auto window = area.toFloat ();

	g.setColour (theme::card ().darker (0.05f));
	g.fillRoundedRectangle (window, kWindowRadius);

	{
		juce::Graphics::ScopedSaveState clip (g);
		g.reduceClipRegion (window.toNearestInt ());

		juce::ColourGradient shade (juce::Colours::black.withAlpha (kWindowShadeAlpha),
									window.getX (), window.getY (),
									juce::Colours::transparentBlack,
									window.getX (), window.getY () + kWindowShadeDepth, false);
		g.setGradientFill (shade);
		g.fillRoundedRectangle (window, kWindowRadius);
	}

	g.setColour (theme::cardBorder ().withAlpha (0.7f));
	g.drawRoundedRectangle (window.reduced (0.5f), kWindowRadius, 1.0f);

	area = area.reduced (kCycleInsetX, kCycleInsetY);

	std::vector<float> cycle = channel.wave;

	if (cycle.empty ())
	{
		cycle = waveform::cycleFor (channel.waveKind, channel.duty);
	}

	if (cycle.size () < 2)
	{
		g.setColour (theme::secondaryText ());
		g.setFont (juce::Font (juce::FontOptions (10.5f)));
		g.drawText ("-", area, juce::Justification::centred, false);
		return;
	}

	juce::Path path;
	const auto count = (int) cycle.size ();
	const float half = (float) area.getHeight () * 0.5f - 2.0f;
	const float span = (float) area.getWidth () / (float) count;

	for (int i = 0; i < count; ++i)
	{
		const float level = juce::jlimit (-1.0f, 1.0f, cycle[(std::size_t) i]);
		const float y = (float) area.getCentreY () - level * half;
		const float x0 = (float) area.getX () + span * (float) i;

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

	g.setColour (theme::accent ().withAlpha (channel.active ? 0.9f : 0.35f));
	g.strokePath (path, juce::PathStrokeType (1.0f));
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
