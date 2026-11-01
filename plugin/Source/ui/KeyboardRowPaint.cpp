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
					  bool waveHovered,
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

	paintWave (g, row.wave, channel, cache.wave, waveHovered);

	if (paintKeys != nullptr)
	{
		// A lit key means "this note is sounding", the same thing the waveform's colour means and
		// the same thing `audible` above decides. Passing the note alone kept the key lit after the
		// sound had stopped, because a decoder's pitch registers go on holding their last value -
		// most visibly on a drum, whose note comes from the song's sample table rather than from a
		// register that ever clears.
		paintKeys (g, row.keys, audible ? channel.note : -1);
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
	/** How many points a row draws, whatever the cycle's own length is.

		A stored table is at most 256, so this only bites on samples: a DPCM hit is up to 32648
		points and the window is about a hundred and twenty pixels wide, where that many segments
		overlap into a smear. The file keeps every point - this decides only what a row can show.
	*/
	constexpr std::size_t kDisplayPoints = 128;

	/** Builds one cycle's line in coordinates local to its window - origin at the top left, no
		knowledge of where the window is on screen.

		That distinction is the whole point. When this took a rectangle it wrote absolute positions
		into the path, and the cache key was the window's size - so scrolling the list moved the
		window, the size stayed the same, the cache was still thought good, and the line was stroked
		at the row's previous height. Two lines per row, one of them forty pixels below the other.
	*/
	/** One column wide per pixel, filled between its quietest and loudest point.

		A filled column is how a sampler draws a sample: the ribbon is the range the sound swung
		through in that pixel, so a decay narrows and a sustain stays wide. Drawn as a line through
		the peaks instead, the same data reads as a thin squiggle and the size is lost.
	*/
	void buildFilledPath (juce::Path& path, int width, int height,
						 const std::vector<playback::waveform::Column>& columns)
	{
		path.clear ();

		const auto count = (int) columns.size ();

		if (count == 0)
		{
			return;
		}

		const float half = (float) height * 0.5f - 2.0f;
		const float span = (float) width / (float) count;

		const auto yOf = [height, half] (float level)
		{
			return (float) height * 0.5f - juce::jlimit (-1.0f, 1.0f, level) * half;
		};

		// The upper edge, left to right.
		for (int i = 0; i < count; ++i)
		{
			const float x = span * (float) i;

			if (i == 0)
			{
				path.startNewSubPath (x, yOf (columns[(std::size_t) i].max));
			}
			else
			{
				path.lineTo (x, yOf (columns[(std::size_t) i].max));
			}
		}

		// And the lower edge back, so the fill closes on itself. A column whose two values are the
		// same - one point of a stored table, or a sample that did not move - has no height to fill,
		// so it is given a pixel: the row would otherwise show nothing where the wave is flat.
		for (int i = count - 1; i >= 0; --i)
		{
			const float top = yOf (columns[(std::size_t) i].max);
			const float bottom = (std::max) (yOf (columns[(std::size_t) i].min), top + 1.0f);

			path.lineTo (span * (float) i, bottom);
		}

		path.closeSubPath ();
	}

	/** Fills the ribbon a recorded sample traced, rather than drawing its outline.

		The path arrives closed - an upper edge and a lower one - because a sample has a range in
		every pixel and not a single value. Stroking it would draw the outline of the ribbon, which is
		a thin line with a fill-shaped hole in it.
	*/
	void fillCycle (juce::Graphics& g, const juce::Path& path, juce::Rectangle<int> area)
	{
		juce::Graphics::ScopedSaveState saved (g);
		g.addTransform (juce::AffineTransform::translation ((float) area.getX (), (float) area.getY ()));
		g.fillPath (path);
	}

	/** Draws a synthesised cycle as steps, one horizontal span per point.

		A generated waveform holds a value for a whole sample period and then jumps - the 2A03's pulse
		output really is a staircase, not a line through its corners. Joining the corners would draw a
		shape the chip never produces, and the difference is visible: a triangle wave read as steps
		has flat tops where the real one has them, and read as a line it has none.

		Kept separate from `buildFilledPath` because the two answer different questions: a recorded
		sample has a range inside each pixel and is filled, a synthesised one has a single value and
		is stepped.
	*/
	void buildStepPath (juce::Path& path, int width, int height, const std::vector<float>& cycle)
	{
		path.clear ();

		const auto count = (int) cycle.size ();

		if (count == 0)
		{
			return;
		}

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

			// The horizontal span is what makes it a step rather than a corner.
			path.lineTo (x0 + span, y);
		}
	}

	/** Strokes a stepped cycle where `area` says it belongs. */
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
				const playback::ChannelState& channel, WavePathCache& cache, bool hovered)
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

	// Two strokes rather than a blur: a ring outside the window in the accent at low alpha, then the
	// hairline in the accent itself. It reads as a glow at this size and costs nothing per frame -
	// a SoftShadow tile per row would be an image to build and blit for a hover state.
	if (hovered)
	{
		g.setColour (theme::accent ().withAlpha (0.30f));
		g.drawRoundedRectangle (window.expanded (1.5f), kWindowRadius + 1.5f, 2.5f);

		g.setColour (theme::accent ());
		g.drawRoundedRectangle (window.reduced (0.5f), kWindowRadius, 1.5f);
	}
	else
	{
		g.setColour (theme::cardBorder ().withAlpha (0.7f));
		g.drawRoundedRectangle (window.reduced (0.5f), kWindowRadius, 1.0f);
	}

	area = area.reduced (kCycleInsetX, kCycleInsetY);

	// A cycle the snapshot carries is the core's own and changes as it plays, so it is drawn as
	// given. The one derived from the registers is a function of three values and the window's
	// size - and it was being rebuilt thirty times a second anyway: a vector copy, a fresh
	// juce::Path and a stroke, for a line that had not moved.
	if (! channel.waveValues ().empty ())
	{
		if (channel.waveValues ().size () < 2)
		{
			paintNoCycle (g, area);
			return;
		}

		juce::Path path;
		const auto& values = channel.waveValues ();

		g.setColour (theme::accent ().withAlpha (channel.active ? 0.9f : 0.35f));

		// Two shapes, decided by what the wave *is* rather than by how long it happens to be.
		//
		// A wavetable holds one level per point until the next point replaces it, so it is a
		// staircase. Drawing it as a ribbon would give each column no height to fill - a table of
		// sixty-four points in a hundred and twenty-eight columns has one point per two columns -
		// and the row would collapse to a one-pixel line sloping between corners, a shape the
		// hardware never makes.
		//
		// A recorded sample is a stream of levels, thousands of them, so each column stands for a
		// slice and carries its range. Filled, that is the sample's shape from attack to decay.
		//
		// `sampleRate` is the flag for "this channel plays recorded bytes": it is set by the two
		// chips that decode samples and by no wavetable chip, and it is what the export uses for the
		// same distinction. Length would not do - a short DPCM hit is shorter than an N163 table.
		const bool recorded = channel.sampleRate > 0.0f;

		if (recorded && values.size () > kDisplayPoints)
		{
			buildFilledPath (path, area.getWidth (), area.getHeight (),
							 playback::waveform::decimateForDisplay (values, kDisplayPoints));
			fillCycle (g, path, area);
		}
		else
		{
			buildStepPath (path, area.getWidth (), area.getHeight (), values);
			strokeCycle (g, path, area);
		}

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
			// A generated wave is a value per sample period and then a jump, so it is drawn as steps.
			// Filling it would be a ribbon the size of one pixel, and joining the corners would draw
			// a slope where the chip holds flat - both say something untrue about the shape.
			buildStepPath (cache.path, area.getWidth (), area.getHeight (), cycle);
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

	// A stepped path is an outline, so it is stroked; the filled ribbon above is the other case.
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
