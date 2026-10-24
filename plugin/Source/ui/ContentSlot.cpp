#include "ContentSlot.h"

#include "ButtonPaint.h"
#include "MixerPanel.h"
#include "Theme.h"
#include "i18n/LanguageManager.h"
#include "playback/PlaybackEngine.h"
#include "playback/WaveExport.h"
#include "settings/SettingsStore.h"

#include <algorithm>
#include <cmath>
#include <functional>

using namespace toybox::ui;

namespace
{
	constexpr int kInset = 20;
	constexpr int kHeader = 44;
	constexpr int kRefreshHz = 30;

	// The keyboard row: one line per channel, five columns.
	constexpr int kRowHeight = 40;
	constexpr int kGroupHeader = 26;
	constexpr int kGroupGap = 10;
	constexpr int kNameWidth = 72;
	constexpr int kButtonWidth = 26;
	constexpr int kNoteWidth = 56;
	constexpr int kWaveWidth = 88;
	constexpr int kMeterWidth = 96;

	juce::String noteName (int note)
	{
		static const char* const names[] = { "C", "C#", "D", "D#", "E", "F",
											 "F#", "G", "G#", "A", "A#", "B" };

		if (note < 0)
		{
			return "-";
		}

		return juce::String (names[note % 12]) + juce::String (note / 12 - 1);
	}
} // namespace

//==============================================================================
/**
	The keyboard: one row per channel, grouped by the chip that plays it.

	Each row carries the channel's name, mute and solo, its note, one cycle of its shape, a
	keyboard with the sounding key lit, and a level meter. Chips that read a table show it; chips
	that generate their shape get a drawn one, because a pulse is a duty setting rather than bytes
	in memory and leaving that column empty would suggest the chip had nothing to show.

	The mute and solo buttons are not this panel's state: they call into the engine's own track
	mute, so the mixer and this view cannot disagree about what is silent.
*/
class ContentSlot::KeyboardPanel : public juce::Component
{
public:
	KeyboardPanel (toybox::PlaybackEngine& engineToUse, juce::String& statusToUse)
		: engine (engineToUse), exportStatus (statusToUse)
	{
		exportButton.setButtonText (toybox::i18n::tr ("visualiser.export"));
		exportButton.setColour (juce::TextButton::buttonColourId, theme::card ());
		exportButton.setColour (juce::TextButton::textColourOffId, theme::primaryText ());
		exportButton.onClick = [this] { if (onExport) { onExport (); } };
		addAndMakeVisible (exportButton);
	}

	/** Back to the default window: a new song has its own range and no manual shift. */
	void resetRange (void)
	{
		observedMin = -1;
		observedMax = -1;
		manualBase = -1;
	}

	void setSnapshot (const toybox::playback::ChipStateSnapshot& snapshotToUse)
	{
		snapshot = snapshotToUse;

		// Peak hold: the level a meter is asked for is the level you just heard, and a bar that
		// falls as fast as the signal rises is unreadable on a fast passage.
		if (peakHold.size () != snapshot.channels.size ())
		{
			peakHold.assign (snapshot.channels.size (), 0.0f);
		}

		for (std::size_t i = 0; i < peakHold.size () && i < snapshot.channels.size (); ++i)
		{
			peakHold[i] = juce::jmax (snapshot.channels[i].volume, peakHold[i] * 0.94f);
		}

		for (const auto& channel : snapshot.channels)
		{
			if (channel.note < 0)
			{
				continue;
			}

			observedMin = observedMin < 0 ? channel.note : juce::jmin (observedMin, channel.note);
			observedMax = observedMax < 0 ? channel.note : juce::jmax (observedMax, channel.note);
		}
	}

	void refreshLanguage (void)
	{
		exportButton.setButtonText (toybox::i18n::tr ("visualiser.export"));
		repaint ();
	}

	void paint (juce::Graphics& g) override
	{
		auto header = getLocalBounds ().reduced (kInset, 0).removeFromTop (kHeader);

		g.setColour (theme::primaryText ());
		g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
		g.drawText (toybox::i18n::tr ("visualiser.keyboard"), header.removeFromLeft (200).withTrimmedTop (10),
					juce::Justification::centredLeft, false);

		// The window controls live in the header so they never scroll away, and to the left of the
		// export button so they sit where this page keeps its other actions.
		paintShiftControls (g, header.withTrimmedTop (8).withHeight (26));

		if (exportStatus.isNotEmpty ())
		{
			g.setColour (theme::secondaryText ());
			g.setFont (juce::Font (juce::FontOptions (11.5f)));
			g.drawText (exportStatus, header.withTrimmedRight (124),
						juce::Justification::centredRight, false);
		}

		rows.clear ();
		buildRows (rows);
		contentHeight = rows.empty () ? 0 : rows.back ().area.getBottom () - getLocalBounds ().getY () - kHeader;

		if (snapshot.channels.empty ())
		{
			paintEmptyState (g);
			return;
		}

		// Clip to the viewport: a row scrolled past the header must not paint over it.
		juce::Graphics::ScopedSaveState clip (g);
		g.reduceClipRegion (getLocalBounds ().withTrimmedTop (kHeader));

		for (const auto& row : rows)
		{
			if (row.area.getBottom () < kHeader || row.area.getY () > getHeight ())
			{
				continue;
			}

			if (row.header)
			{
				paintGroupHeader (g, row);
			}
			else
			{
				paintChannelRow (g, row);
			}
		}
	}

	void resized (void) override
	{
		exportButton.setBounds (getWidth () - kInset - 112, 10, 112, 24);
		clampScroll ();
	}

	void mouseDown (const juce::MouseEvent& event) override
	{
		const auto point = event.getPosition ();

		if (shiftLeftRect.contains (point)) { shiftOctaves (-1); return; }
		if (shiftRightRect.contains (point)) { shiftOctaves (1); return; }

		if (autoRect.contains (point))
		{
			// Back to following the song, and back to the setting's answer if it named a note.
			manualBase = -1;
			repaint ();
			return;
		}

		for (const auto& row : rows)
		{
			if (row.header || row.channel < 0 || ! row.area.contains (point))
			{
				continue;
			}

			const auto& channel = snapshot.channels[(std::size_t) row.channel];

			if (channel.trackIndex < 0)
			{
				return;
			}

			// The arrow is a button: it already says which way the note went, so going there
			// costs nothing and needs no second control.
			if (row.keys.contains (point))
			{
				const bool left = point.x < row.keys.getX () + 14;
				const bool right = point.x > row.keys.getRight () - 14;

				if (left || right)
				{
					shiftOctaves (left ? -1 : 1);
					return;
				}
			}

			if (row.mute.contains (point))
			{
				engine.setTrackMute ((std::size_t) channel.trackIndex,
									 ! engine.isTrackMuted ((std::size_t) channel.trackIndex));
				repaint ();
			}
			else if (row.solo.contains (point))
			{
				engine.setTrackSolo ((std::size_t) channel.trackIndex,
									 ! engine.isTrackSoloed ((std::size_t) channel.trackIndex));
				repaint ();
			}

			return;
		}
	}

	void mouseMove (const juce::MouseEvent& event) override
	{
		const auto point = event.getPosition ();
		const int hovered = shiftLeftRect.contains (point) ? 0
						  : (shiftRightRect.contains (point) ? 1 : -1);

		if (hovered != hoveredControl)
		{
			hoveredControl = hovered;
			repaint (getLocalBounds ().withHeight (kHeader));
		}
	}

	void mouseExit (const juce::MouseEvent&) override
	{
		if (hoveredControl != -1)
		{
			hoveredControl = -1;
			repaint (getLocalBounds ().withHeight (kHeader));
		}
	}

	void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
	{
		if (event.mods.isCtrlDown ())
		{
			if (std::abs (wheel.deltaY) > 0.01f)
			{
				shiftOctaves (wheel.deltaY > 0.0f ? 1 : -1);
			}

			return;
		}

		// Scrolling by hand rather than through a Viewport: the panel draws only the rows that are
		// on screen, so it already knows where everything is and a viewport would be a second
		// authority on the same geometry.
		scrollOffset -= (int) std::lround (wheel.deltaY * (float) kRowHeight * 3.0f);
		clampScroll ();
		repaint ();
	}

	std::function<void ()> onExport;

private:
	struct Row
	{
		bool header = false;
		juce::String title;
		int channel = -1;

		juce::Rectangle<int> area, name, mute, solo, note, wave, keys, meter;
	};

	void clampScroll (void)
	{
		const int visible = juce::jmax (0, getHeight () - kHeader);
		scrollOffset = juce::jlimit (0, juce::jmax (0, contentHeight - visible), scrollOffset);
	}

	void buildRows (std::vector<Row>& out) const
	{
		if (snapshot.channels.empty ())
		{
			return;
		}

		const int left = kInset;
		const int width = juce::jmax (200, getWidth () - 2 * kInset);
		int y = kHeader - scrollOffset;

		juce::String group;

		for (std::size_t i = 0; i < snapshot.channels.size (); ++i)
		{
			const auto& channel = snapshot.channels[i];
			const auto chip = juce::String (channel.chipName);

			if (chip != group)
			{
				group = chip;

				Row header;
				header.header = true;
				header.title = chip;
				header.area = { left, y, width, kGroupHeader };
				out.push_back (header);

				y += kGroupHeader + 2;
			}

			Row row;
			row.channel = (int) i;
			row.area = { left, y, width, kRowHeight };

			auto inner = row.area.reduced (0, 4);
			row.name = inner.removeFromLeft (kNameWidth);

			inner.removeFromLeft (6);
			row.mute = inner.removeFromLeft (kButtonWidth).reduced (0, 6);
			inner.removeFromLeft (4);
			row.solo = inner.removeFromLeft (kButtonWidth).reduced (0, 6);
			inner.removeFromLeft (10);

			row.note = inner.removeFromLeft (kNoteWidth);
			inner.removeFromLeft (6);

			row.wave = inner.removeFromLeft (kWaveWidth);
			inner.removeFromLeft (10);

			row.meter = inner.removeFromRight (kMeterWidth);
			inner.removeFromRight (10);

			row.keys = inner;

			out.push_back (row);

			y += kRowHeight + 2;

			// a gap between groups, once the next channel belongs to another chip
			const bool lastOfGroup = (i + 1 >= snapshot.channels.size ())
									 || juce::String (snapshot.channels[i + 1].chipName) != chip;

			if (lastOfGroup)
			{
				y += kGroupGap;
			}
		}
	}

	void paintEmptyState (juce::Graphics& g)
	{
		auto area = getLocalBounds ().reduced (kInset, 0).withTrimmedTop (kHeader);

		juce::String chips;

		for (const auto& name : snapshot.chips)
		{
			if (chips.isNotEmpty ()) { chips << " / "; }
			chips << name;
		}

		if (chips.isNotEmpty ())
		{
			g.setColour (theme::primaryText ());
			g.setFont (juce::Font (juce::FontOptions (13.0f)));
			g.drawText (chips, area.removeFromTop (26), juce::Justification::centredTop, false);
		}

		g.setColour (theme::secondaryText ());
		g.setFont (juce::Font (juce::FontOptions (12.5f)));
		g.drawText (toybox::i18n::tr (chips.isNotEmpty () ? "visualiser.keyboardUnsupported"
														  : "visualiser.keyboardUnavailable"),
					area.removeFromTop (24), juce::Justification::centredTop, false);
	}

	void paintGroupHeader (juce::Graphics& g, const Row& row)
	{
		const int count = juce::jlimit (0, 32, (int) std::count_if (snapshot.channels.begin (), snapshot.channels.end (),
																   [&row] (const auto& channel)
																   { return juce::String (channel.chipName) == row.title; }));

		auto area = row.area;
		g.setColour (theme::secondaryText ().withAlpha (0.35f));
		g.fillRoundedRectangle (area.toFloat (), 3.0f);

		g.setColour (theme::primaryText ());
		g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
		g.drawText (row.title, area.reduced (10, 0), juce::Justification::centredLeft, false);

		g.setColour (theme::secondaryText ());
		g.setFont (juce::Font (juce::FontOptions (11.0f)));
		g.drawText (juce::String (count) + " ch", area.reduced (10, 0),
					juce::Justification::centredRight, false);
	}

	void paintChannelRow (juce::Graphics& g, const Row& row)
	{
		const auto& channel = snapshot.channels[(std::size_t) row.channel];
		const bool audible = channel.active && channel.volume > 0.001f;

		// A tile per row, the material the mixer strips use, with its shadow drawn through the
		// same helper the cards use - a real gaussian blur, not a second rectangle pretending to
		// be one. The shadow stays inside the row's own height, so nothing gets cut off.
		const auto tile = row.area.reduced (0, 2).toFloat ();
		drawSoftShadow (g, tile, theme::buttonRadius, theme::cardKeyShadow (), 6, juce::Point<int> (0, 2));
		g.setColour (theme::track ().withAlpha (0.32f));
		g.fillRoundedRectangle (tile, theme::buttonRadius);
		g.setColour (theme::cardBorder ().withAlpha (0.6f));
		g.drawRoundedRectangle (tile.reduced (0.5f), theme::buttonRadius, 1.0f);

		g.setColour (audible ? theme::primaryText () : theme::secondaryText ());
		g.setFont (juce::Font (juce::FontOptions (12.0f)));
		g.drawText (juce::String (channel.name), row.name, juce::Justification::centredLeft, false);

		paintToggle (g, row.mute, "M", channel.trackIndex >= 0
					 && engine.isTrackMuted ((std::size_t) channel.trackIndex), true);
		paintToggle (g, row.solo, "S", channel.trackIndex >= 0
					 && engine.isTrackSoloed ((std::size_t) channel.trackIndex), false);

		// the note, or a dash when the channel has no pitch to speak of
		g.setColour (channel.note >= 0 ? theme::primaryText () : theme::secondaryText ());
		g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
		g.drawText (noteName (channel.note), row.note, juce::Justification::centred, false);

		paintWave (g, row.wave, channel);
		paintKeyboard (g, row.keys, channel.note);
		paintMeter (g, row.meter, channel, peakHold[(std::size_t) row.channel]);
	}

	/** Mute and solo, drawn with the mixer's own face and colours: mute fills red, solo amber,
		and a toggled one carries the matching glow. Anything else here would make the two views
		look like different applications. */
	void paintToggle (juce::Graphics& g, juce::Rectangle<int> area, const char* letter, bool on, bool mute)
	{
		if (on)
		{
			drawButtonGlow (g, area, mute ? theme::muteGlow () : theme::soloGlow (), theme::compactGlowBlur);
		}

		ButtonFace face;
		face.level = on ? ButtonLevel::filled : ButtonLevel::material;
		face.toggled = on;
		face.fill = mute ? theme::mute () : theme::solo ();

		drawButtonFace (g, area.toFloat (), face);

		g.setColour (on ? (mute ? theme::onAccent () : theme::primaryText ()) : theme::secondaryText ());
		g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
		g.drawText (letter, area, juce::Justification::centred, false);
	}

	/** The octave shift, and the note the window currently starts at. */
	void paintShiftControls (juce::Graphics& g, juce::Rectangle<int> area)
	{
		if (area.getWidth () < 300)
		{
			shiftLeftRect = shiftRightRect = autoRect = {};
			return;
		}

		auto strip = area.withTrimmedRight (124 + 6);

		autoRect = strip.removeFromRight (54);
		strip.removeFromRight (4);
		shiftRightRect = strip.removeFromRight (26);
		strip.removeFromRight (4);
		shiftLeftRect = strip.removeFromRight (26);

		const int base = lastSpan.base;
		const bool canGoDown = base > kFirstMusicalNote;
		const bool canGoUp = base + 12 * 8 <= kLastMusicalNote;

		const auto drawShift = [&] (juce::Rectangle<int> bounds, const juce::String& glyph,
									bool enabled, int index)
		{
			ButtonFace face;
			face.level = ButtonLevel::material;
			face.enabled = enabled;
			face.hovered = (hoveredControl == index);

			drawButtonFace (g, bounds.toFloat (), face);

			g.setColour (enabled ? theme::primaryText () : theme::secondaryText ().withAlpha (0.35f));
			g.setFont (juce::Font (juce::FontOptions (10.0f)));
			g.drawText (glyph, bounds, juce::Justification::centred, false);
		};

		drawShift (shiftLeftRect, juce::String::charToString ((juce::juce_wchar) 0x25C0), canGoDown, 0);
		drawShift (shiftRightRect, juce::String::charToString ((juce::juce_wchar) 0x25B6), canGoUp, 1);

		// The starting note doubles as the way back to automatic: it is the state a manual shift
		// replaced, so showing it there costs no extra control and answers "why did it stop
		// following the song?".
		const bool automatic = toybox::settings::SettingsStore::getInstance ().getKeyboardStartNote () < 0
							   && manualBase < 0;

		g.setColour (automatic ? theme::secondaryText () : theme::accent ());
		g.setFont (juce::Font (juce::FontOptions (11.0f)));
		g.drawText (noteName (base), autoRect, juce::Justification::centred, false);
	}


	void paintWave (juce::Graphics& g, juce::Rectangle<int> area, const toybox::playback::ChannelState& channel)
	{
		std::vector<float> cycle = channel.wave;

		if (cycle.empty ())
		{
			cycle = synthesisedCycle (channel);
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

	/** A cycle for the channels that generate their shape rather than read it. */
	std::vector<float> synthesisedCycle (const toybox::playback::ChannelState& channel) const
	{
		std::vector<float> cycle;

		switch (channel.waveKind)
		{
			case 2:		// pulse: high for the duty setting, low for the rest
			{
				constexpr int points = 32;
				const int high = juce::jlimit (1, points - 1, (int) std::lround ((double) channel.duty * points));

				for (int i = 0; i < points; ++i)
				{
					cycle.push_back (i < high ? 1.0f : -1.0f);
				}

				break;
			}

			case 3:		// triangle: 32 steps down and back up
			{
				constexpr int points = 32;

				for (int i = 0; i < points; ++i)
				{
					const int step = i < points / 2 ? i : points - 1 - i;
					cycle.push_back ((float) step / (float) (points / 2 - 1) * 2.0f - 1.0f);
				}

				break;
			}

			case 4:		// noise: a short burst from the same feedback the chip uses
			{
				std::uint32_t shift = 1;

				for (int i = 0; i < 32; ++i)
				{
					const std::uint32_t bit = ((shift >> 0) ^ (shift >> 1)) & 1u;
					shift = (shift >> 1) | (bit << 14);
					cycle.push_back ((shift & 1u) ? 1.0f : -1.0f);
				}

				break;
			}

			case 5:		// delta: one bit per step, integrated - the shape the DAC produces
			{
				float level = 0.0f;

				for (int i = 0; i < 32; ++i)
				{
					level += (i % 7 == 0) ? -0.6f : 0.2f;
					level = juce::jlimit (-1.0f, 1.0f, level);
					cycle.push_back (level);
				}

				break;
			}

			default:
				break;
		}

		return cycle;
	}

	/** Keys are a fixed width. A wider window shows more of them rather than bigger ones, which
		is what a keyboard is: the distance between two keys does not change because the window
		did. */
	static constexpr int kWhiteKeyWidth = 20;
	static constexpr int kMinWhiteKeyWidth = 16;
	static constexpr int kFirstMusicalNote = 12;	// C0
	static constexpr int kLastMusicalNote = 119;	// B8

	/** What one row's keyboard covers, worked out from the room it has. */
	struct KeySpan
	{
		int base = 36;			// lowest note drawn
		int whiteKeys = 28;
		int keyWidth = kWhiteKeyWidth;
		int lastNote = 83;
	};

	static int octaveFloor (int note)
	{
		return note - ((note % 12) + 12) % 12;
	}

	/** The note of the k-th white key counted from `base`, which is itself a C. */
	static int noteForWhite (int base, int k)
	{
		static const int offsets[7] = { 0, 2, 4, 5, 7, 9, 11 };
		return base + (k / 7) * 12 + offsets[k % 7];
	}

	/** Where the keyboard starts.

		Three answers, in order of authority: the setting, if the user named a note; the manual
		shift, if they moved the window; otherwise the notes this song has played so far, taking
		the lowest of them and rounding down to a C.
	*/
	int baseNote (int octavesVisible) const
	{
		const int setting = toybox::settings::SettingsStore::getInstance ().getKeyboardStartNote ();

		if (setting >= 0)
		{
			return setting;
		}

		if (manualBase >= 0)
		{
			return manualBase;
		}

		if (observedMin < 0)
		{
			return 36;		// C2 until the song has played something
		}

		const int low = octaveFloor (observedMin);

		if (observedMax - low < octavesVisible * 12)
		{
			return juce::jlimit (0, kLastMusicalNote - 12, low);
		}

		// The music is wider than the window. Centre on its median so both edges run out at
		// roughly the same time, rather than pinning the lowest note and losing the top half.
		const int mid = (observedMin + observedMax) / 2;

		return juce::jlimit (0, kLastMusicalNote - octavesVisible * 12,
							 octaveFloor (mid) - (octavesVisible / 2) * 12);
	}

	KeySpan spanFor (juce::Rectangle<int> area) const
	{
		KeySpan span;

		const int minWhite = 14;		// two octaves, however narrow the window gets

		span.keyWidth = kWhiteKeyWidth;

		if (area.getWidth () / span.keyWidth < minWhite)
		{
			span.keyWidth = juce::jmax (10, area.getWidth () / minWhite);
		}

		span.whiteKeys = juce::jmax (7, area.getWidth () / span.keyWidth);

		const int octavesVisible = juce::jmax (2, span.whiteKeys / 7);
		span.base = baseNote (octavesVisible);

		// Nothing above B8 can light, so the keys stop there and the rest is left blank.
		const int whiteByRange = ((kLastMusicalNote - span.base) / 12) * 7 + 7;
		span.whiteKeys = juce::jmin (span.whiteKeys, whiteByRange);

		span.lastNote = noteForWhite (span.base, span.whiteKeys - 1);

		return span;
	}

	/** A fixed-width keyboard starting at the span's base note, with the sounding key lit. */
	void paintKeyboard (juce::Graphics& g, juce::Rectangle<int> area, int note)
	{
		if (area.getWidth () < 60 || area.getHeight () < 12)
		{
			return;
		}

		const auto span = spanFor (area);
		lastSpan = span;

		const float whiteWidth = (float) span.keyWidth;
		const float blackWidth = whiteWidth * 0.62f;
		const float blackHeight = (float) area.getHeight () * 0.62f;

		// Opaque colours. A translucent black key let the white keys' divider lines show through
		// it, which looked like a printing fault rather than like a keyboard.
		const juce::Colour whiteKey = theme::card ().interpolatedWith (theme::primaryText (), 0.09f);
		const juce::Colour blackKey = theme::card ().interpolatedWith (theme::primaryText (), 0.74f);
		const juce::Colour litKey = theme::accent ();
		const juce::Colour litBlack = theme::accent ().darker (0.25f);

		const auto drawKey = [&] (int keyNote, juce::Rectangle<float> key, bool black)
		{
			const bool lit = keyNote == note;

			g.setColour (lit ? (black ? litBlack : litKey) : (black ? blackKey : whiteKey));
			g.fillRoundedRectangle (key, 1.5f);

			if (lit && key.getWidth () >= 13.0f)
			{
				g.setColour (theme::onAccent ());
				g.setFont (juce::Font (juce::FontOptions (9.0f)));
				g.drawText (noteName (keyNote), key.toNearestInt (), juce::Justification::centred, false);
			}
		};

		// White keys first, then the blacks on top of them.
		for (int k = 0; k < span.whiteKeys; ++k)
		{
			const int keyNote = noteForWhite (span.base, k);
			const juce::Rectangle<float> key ((float) area.getX () + whiteWidth * (float) k,
											  (float) area.getY (), whiteWidth - 1.0f,
											  (float) area.getHeight ());
			drawKey (keyNote, key, false);
		}

		for (int k = 0; k < span.whiteKeys; ++k)
		{
			const int pc = k % 7;

			// E and B have no black key above them; the rest do.
			if (pc == 2 || pc == 6) { continue; }

			const int keyNote = noteForWhite (span.base, k) + 1;

			if (keyNote > kLastMusicalNote) { continue; }

			const juce::Rectangle<float> key ((float) area.getX () + whiteWidth * (float) (k + 1) - blackWidth * 0.5f,
											  (float) area.getY (), blackWidth, blackHeight);
			drawKey (keyNote, key, true);
		}

		// The octave names, on the C keys. Fewer of them as the keys get small, because a label
		// that collides with its neighbour is worse than no label at all.
		const int everyNth = whiteWidth >= 15.0f ? 1 : (whiteWidth >= 10.0f ? 2 : 0);

		if (everyNth > 0)
		{
			g.setFont (juce::Font (juce::FontOptions (8.5f)));
			int nth = 0;

			for (int k = 0; k < span.whiteKeys; ++k)
			{
				const int keyNote = noteForWhite (span.base, k);

				if (keyNote % 12 != 0)
				{
					continue;
				}

				if (keyNote == note)
				{
					continue;		// the row's own note label already names this key
				}

				if ((nth++ % everyNth) != 0)
				{
					continue;
				}

				const juce::Rectangle<float> key ((float) area.getX () + whiteWidth * (float) k + 1.0f,
												  (float) area.getY (), whiteWidth - 2.0f,
												  (float) area.getHeight ());

				g.setColour (theme::secondaryText ().withAlpha (0.75f));
				g.drawText (noteName (keyNote), key.toNearestInt ().withTrimmedBottom (1),
							juce::Justification::centredBottom, false);
			}
		}

		// Out of range: the note is further along the keyboard than the keys on screen. The arrow
		// points that way and, when clicked, moves the window there.
		if (note >= 0 && (note < span.base || note > span.lastNote)
			&& toybox::settings::SettingsStore::getInstance ().getKeyboardRangeHints ())
		{
			const bool above = note > span.lastNote;
			const float size = 4.0f;
			const float cx = above ? (float) area.getRight () - 6.0f : (float) area.getX () + 6.0f;
			const float cy = (float) area.getCentreY ();

			juce::Path arrow;

			if (above)
			{
				arrow.addTriangle (cx + size, cy, cx - size, cy - size, cx - size, cy + size);
			}
			else
			{
				arrow.addTriangle (cx - size, cy, cx + size, cy - size, cx + size, cy + size);
			}

			g.setColour (theme::accent ().withAlpha (0.9f));
			g.fillPath (arrow);

			g.setColour (theme::secondaryText ());
			g.setFont (juce::Font (juce::FontOptions (8.5f)));
			g.drawText (noteName (note), area.withTrimmedLeft (above ? 0 : 12).withTrimmedRight (above ? 12 : 0),
						juce::Justification::centred, false);
		}
	}

	/** Moves the window by whole octaves; the setting and the song's observed range both yield
		to it until the next song. */
	void shiftOctaves (int direction)
	{
		const int step = direction > 0 ? 12 : -12;
		manualBase = juce::jlimit (kFirstMusicalNote, kLastMusicalNote - 12 * 3, lastSpan.base + step);
		repaint ();
	}

	void paintMeter (juce::Graphics& g, juce::Rectangle<int> area, const toybox::playback::ChannelState& channel,
					 float peak)
	{
		auto track = area.reduced (0, 8);

		g.setColour (theme::cardBorder ());
		g.fillRoundedRectangle (track.toFloat (), 2.0f);

		const float level = juce::jlimit (0.0f, 1.0f, channel.volume);

		if (level > 0.001f)
		{
			g.setColour (level > 0.9f ? juce::Colour (0xffd0564b)
									   : theme::accent ().withAlpha (0.85f));
			g.fillRoundedRectangle (track.toFloat ().withWidth ((float) track.getWidth () * level), 2.0f);
		}

		if (peak > 0.01f)
		{
			const float x = (float) track.getX () + (float) track.getWidth () * juce::jlimit (0.0f, 1.0f, peak);
			g.setColour (theme::primaryText ().withAlpha (0.5f));
			g.fillRect (juce::Rectangle<float> (x - 0.5f, (float) track.getY (), 1.0f, (float) track.getHeight ()));
		}
	}

	toybox::PlaybackEngine& engine;
	toybox::playback::ChipStateSnapshot snapshot;
	std::vector<float> peakHold;
	std::vector<Row> rows;

	int scrollOffset = 0;
	int contentHeight = 0;

	// The range this song has played so far. It only ever grows: a window that shrank whenever a
	// bass note passed would move the whole page, and thirteen rows moving at once is unreadable.
	int observedMin = -1;
	int observedMax = -1;

	// Set when the user moves the window by hand; -1 means "follow the setting or the song".
	int manualBase = -1;

	KeySpan lastSpan;

	// The header's shift controls. Drawn rather than built as child buttons, so they sit in the
	// same material as everything else without a child's clipping deciding where their shadow ends.
	juce::Rectangle<int> shiftLeftRect, shiftRightRect, autoRect;
	int hoveredControl = -1;

	juce::String& exportStatus;
	juce::TextButton exportButton { "export" };
};

//==============================================================================
class ContentSlot::ScopePanel : public juce::Component
{
public:
	void paint (juce::Graphics& g) override
	{
		g.setColour (theme::secondaryText ());
		g.setFont (juce::Font (juce::FontOptions (13.0f)));
		g.drawText (toybox::i18n::tr ("visualiser.scopePlaceholder"), getLocalBounds (),
					juce::Justification::centred, false);
	}
};

//==============================================================================
ContentSlot::ContentSlot (toybox::PlaybackEngine& engineToUse)
	: engine (engineToUse)
{
	mixer = std::make_unique<MixerPanel> (engine);

	keyboardPanel = std::make_unique<KeyboardPanel> (engine, exportStatus);
	keyboardPanel->onExport = [this]
	{
		// The export lives with the panel that owns the waves, but the file chooser is the
		// slot's: one chooser per page rather than one per view.
		const int writable = (int) snapshot.channels.size ();

		if (writable == 0)
		{
			exportStatus = toybox::i18n::tr ("visualiser.exportEmpty");
			keyboardPanel->repaint ();
			return;
		}

		waveChooser = std::make_unique<juce::FileChooser> (toybox::i18n::tr ("visualiser.export"),
														   juce::File::getSpecialLocation (juce::File::userDesktopDirectory),
														   juce::String (), true);

		waveChooser->launchAsync (juce::FileBrowserComponent::openMode
								  | juce::FileBrowserComponent::canSelectDirectories,
								  [this] (const juce::FileChooser& chooser)
		{
			const auto folder = chooser.getResult ();

			if (folder == juce::File () || ! folder.isDirectory ())
			{
				return;
			}

			int written = 0;

			for (std::size_t i = 0; i < snapshot.channels.size (); ++i)
			{
				const auto& channel = snapshot.channels[i];

				if (channel.wave.size () < 2)
				{
					continue;
				}

				const auto name = juce::File::createLegalFileName (juce::String (channel.name)
																   + " " + juce::String ((int) i + 1))
								  + ".wav";

				if (playback::writeCycleWav (channel, folder.getChildFile (name).getFullPathName ().toStdString ()))
				{
					++written;
				}
			}

			exportStatus = juce::String (written) + " WAV";
			keyboardPanel->repaint ();
		});
	};

	scopePanel = std::make_unique<ScopePanel> ();

	addAndMakeVisible (*mixer);
	addChildComponent (*keyboardPanel);
	addChildComponent (*scopePanel);

	startTimerHz (kRefreshHz);
}

ContentSlot::~ContentSlot (void)
{
	stopTimer ();
	engine.setStateCaptureWanted (false);
}

void ContentSlot::setView (ContentView view)
{
	if (view == current && (mixer->isVisible () || keyboardPanel->isVisible () || scopePanel->isVisible ()))
	{
		return;
	}

	current = view;

	mixer->setVisible (view == ContentView::mixer);
	keyboardPanel->setVisible (view == ContentView::keyboard);
	scopePanel->setVisible (view == ContentView::oscilloscope);

	// The capture is only worth running for the panel that is showing.
	engine.setStateCaptureWanted (view == ContentView::keyboard);

	if (view == ContentView::keyboard)
	{
		lastChannelCount = -1;		// force a relayout of the panel on the next tick
	}

	resized ();
	repaint ();
}

void ContentSlot::refresh (void)
{
	if (mixer != nullptr)
	{
		mixer->refresh ();
	}
}

void ContentSlot::refreshLanguage (void)
{
	if (mixer != nullptr) { mixer->refreshLanguage (); }
	if (keyboardPanel != nullptr) { keyboardPanel->refreshLanguage (); }
	if (scopePanel != nullptr) { scopePanel->repaint (); }
}

void ContentSlot::songChanged (void)
{
	if (mixer != nullptr) { mixer->refresh (); }

	snapshot.chips.clear ();
	snapshot.channels.clear ();
	snapshot.valid = false;
	lastChannelCount = -1;
	exportStatus.clear ();

	if (keyboardPanel != nullptr)
	{
		// The panel paints its own copy; clearing only the slot's left the previous song's
		// channels on screen, which is exactly the bug this line fixes.
		keyboardPanel->resetRange ();
		keyboardPanel->setSnapshot (snapshot);
		keyboardPanel->repaint ();
	}
}

void ContentSlot::paint (juce::Graphics& g)
{
	// The mixer is a CardComponent and paints its own rounded card, border and shadow included.
	// Drawing one here as well put a card inside a card, which reads as a doubled outline.
	if (current == ContentView::mixer)
	{
		return;
	}

	// The other two are contents only, so the slot provides the card they sit in.
	g.setColour (theme::card ());
	g.fillRoundedRectangle (getLocalBounds ().toFloat (), theme::cardRadius);
	g.setColour (theme::cardBorder ());
	g.drawRoundedRectangle (getLocalBounds ().toFloat ().reduced (0.5f), theme::cardRadius, 1.0f);
}

void ContentSlot::resized (void)
{
	// The panels sit inside the card's rounded corners; the mixer brings its own card, so it takes
	// the whole slot rather than an inset copy of it.
	const auto inner = getLocalBounds ().reduced (2);

	if (mixer != nullptr) { mixer->setBounds (getLocalBounds ()); }
	if (keyboardPanel != nullptr) { keyboardPanel->setBounds (inner); }
	if (scopePanel != nullptr) { scopePanel->setBounds (inner); }
}

void ContentSlot::timerCallback (void)
{
	if (current != ContentView::keyboard || ! keyboardPanel->isVisible ())
	{
		return;
	}

	if (! isShowing ())
	{
		engine.setStateCaptureWanted (false);
		return;
	}

	engine.setStateCaptureWanted (true);

	if (engine.captureChipState (snapshot))
	{
		keyboardPanel->setSnapshot (snapshot);

		const int rows = (int) snapshot.channels.size ();

		if (rows != lastChannelCount)
		{
			lastChannelCount = rows;
			keyboardPanel->resized ();
		}

		if (isShowing ())
		{
			keyboardPanel->repaint ();
		}
	}
}
