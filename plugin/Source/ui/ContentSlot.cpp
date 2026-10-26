#include "ContentSlot.h"

#include "ButtonPaint.h"
#include "MixerPanel.h"
#include "KeyboardLayout.h"
#include "KeyboardRowLayout.h"
#include "KeyboardRowPaint.h"
#include "WaveShape.h"
#include "Theme.h"
#include "i18n/LanguageManager.h"
#include "playback/PlaybackEngine.h"
#include "playback/WaveExport.h"
#include "settings/SettingsStore.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <functional>

using namespace toybox::ui;

namespace
{
	constexpr int kInset = 20;
	constexpr int kHeader = 44;
	constexpr int kRefreshHz = 30;

	// The row's own measurements live in KeyboardRowLayout: thirteen columns that have to fit
	// a card are arithmetic, and arithmetic belongs where a test can call it.
	keyboard::RowMetrics metrics;

	// The card's own scrollbar, at its right edge - the mixer's width, for the same reason: it is
	// the same gesture in the same kind of card.
	constexpr int kScrollBarWidth = 12;
	constexpr int kScrollBarGap = 6;		// between the bar and the card's edge

	// How tall the "there is more below" fade is, at the card's bottom edge.
	constexpr int kBottomFade = 18;

	/** The shift comes from the setting; the table itself lives in KeyboardLayout so the settings
		page, the note column and the octave labels all name a note the same way. */
	int octaveShiftForNaming (void)
	{
		return octaveShiftForMiddleC (
			toybox::settings::SettingsStore::getInstance ().getKeyboardMiddleCOctave ());
	}

	juce::String noteName (int note, int octaveShift)
	{
		return midiNoteName (note, octaveShift);
	}
} // namespace

//==============================================================================
namespace
{
	const std::vector<ContentViewInfo> viewTable
	{
		{ ContentView::mixer,			"mixer",		"view.mixer" },
		{ ContentView::keyboard,		"keyboard",		"view.keyboard" },
		{ ContentView::oscilloscope,	"oscilloscope",	"view.oscilloscope" }
	};
}

namespace toybox::ui
{

const std::vector<ContentViewInfo>& contentViews (void)
{
	return viewTable;
}

int contentViewCount (void)
{
	return (int) viewTable.size ();
}

ContentView contentViewAt (int index)
{
	return viewTable[(size_t) juce::jlimit (0, contentViewCount () - 1, index)].view;
}

int contentViewIndex (ContentView view)
{
	for (size_t i = 0; i < viewTable.size (); ++i)
	{
		if (viewTable[i].view == view)
		{
			return (int) i;
		}
	}

	return 0;
}

} // namespace toybox::ui

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
class ContentSlot::KeyboardPanel : public juce::Component,
								   private juce::ScrollBar::Listener
{
public:
	KeyboardPanel (toybox::PlaybackEngine& engineToUse, juce::String& statusToUse)
		: engine (engineToUse), exportStatus (statusToUse)
	{
		// No colours set here on purpose: the button is drawn by the application's look and feel,
		// which asks the palette. Overriding them once in the constructor is why this button kept
		// its old colours after a theme change.
		exportButton.setButtonText (toybox::i18n::tr ("visualiser.export"));
		exportButton.onClick = [this] { if (onExport) { onExport (); } };
		setComponentID ("view.keyboard");

		// The real thing, so its hover and drag behaviour come from the same look and feel the
		// mixer's bar uses. An earlier attempt at this did not paint, and the reason was not the
		// component: it was being shown and hidden from inside paint(), and toggling a child's
		// visibility during a paint callback cancels that child's paint for the frame. Everything
		// that touches the bar now happens outside paint - see syncScrollBar().
		verticalBar.setAutoHide (false);
		verticalBar.addListener (this);

		addAndMakeVisible (exportButton);
		addAndMakeVisible (verticalBar);
	}

	~KeyboardPanel (void) override
	{
		verticalBar.removeListener (this);
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

		// A chip's period register holds its last value, and a fast passage has gaps between notes
		// shorter than a display frame. Holding the last note for a configurable moment keeps a row
		// from blinking; it is what the note-off delay in the settings page changes.
		const int holdMs = toybox::settings::SettingsStore::getInstance ().getKeyboardNoteHoldMs ();
		const double now = juce::Time::getMillisecondCounterHiRes ();

		if (held.size () != snapshot.channels.size ())
		{
			held.assign (snapshot.channels.size (), Held {});
		}

		for (std::size_t i = 0; i < snapshot.channels.size (); ++i)
		{
			auto& channel = snapshot.channels[i];

			if (channel.active)
			{
				held[i] = { channel.note, channel.volume, now };
			}
			else if (holdMs > 0 && held[i].at > 0.0 && (now - held[i].at) <= (double) holdMs)
			{
				channel.note = held[i].note;
				channel.volume = held[i].volume;
				channel.active = true;
			}
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
		// A new snapshot can bring a channel the last one did not have, which changes how tall the
		// content is. Rebuilt here rather than in paint(), which only draws what it is given.
		relayout ();
		syncScrollBar ();
	}

	void refreshLanguage (void)
	{
		exportButton.setButtonText (toybox::i18n::tr ("visualiser.export"));
		repaint ();
	}

	void lookAndFeelChanged (void) override
	{
		// The header is drawn with palette colours, so it has to be redrawn when the palette does.
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

		// No relayout() here. It is already called by resized() and by setSnapshot(), which are the
		// two things that can change what the rows are; calling it again on every paint meant the
		// whole row rectangle list was rebuilt twice per frame - once into `rows` for this paint, and
		// once more sixty milliseconds later. Drawing also had the side effect of changing what it
		// would draw next.
		if (snapshot.channels.empty ())
		{
			paintEmptyState (g);
			return;
		}

		// Clip to the viewport: a row scrolled past the header must not paint over it.
		juce::Graphics::ScopedSaveState clip (g);
		g.reduceClipRegion (getLocalBounds ().withTrimmedTop (kHeader));

		const int octaveShift = octaveShiftForNaming ();

		if (waveCaches.size () < snapshot.channels.size ())
		{
			waveCaches.resize (snapshot.channels.size ());
		}

		for (const auto& row : rows)
		{
			if (row.area.getBottom () < kHeader || row.area.getY () > getHeight ())
			{
				continue;
			}

			if (row.header)
			{
				// row.chipCount was counted when the rows were built. This used to run a count_if
				// over every channel, per header, per frame - and built a juce::String per
				// comparison to do it.
				keyboard::paintGroupHeader (g, row, row.chipCount);
			}
			else
			{
				const auto& channel = snapshot.channels[(std::size_t) row.channel];
				const bool muted = channel.trackIndex >= 0
								   && engine.isTrackMuted ((std::size_t) channel.trackIndex);
				const bool soloed = channel.trackIndex >= 0
									&& engine.isTrackSoloed ((std::size_t) channel.trackIndex);

				keyboard::RowHover hover;
				hover.mute = (hoveredRowChannel == row.channel && hoveredRowMute);
				hover.solo = (hoveredRowChannel == row.channel && ! hoveredRowMute);

				// One cache per channel, sized to the snapshot below. The waveform of a channel
				// whose shape comes from its registers is rebuilt only when one of them moves.
				keyboard::WavePathCache& waveCache = waveCaches[(std::size_t) row.channel];

				keyboard::paintChannelRow (g, row, channel, muted, soloed, octaveShift, hover,
										   [this] (juce::Graphics& graphics, juce::Rectangle<int> area, int note)
										   { paintKeyboard (graphics, area, note); },
										   waveCache);
			}
		}

		// A fade along the bottom when there is more to scroll: without it the last row is cut in
		// half by the card's edge, which reads as a layout fault rather than as "keep going".
		const int visible = juce::jmax (0, getHeight () - kHeader);

		if (contentHeight > visible)
		{
			auto bottom = getLocalBounds ().removeFromBottom (kBottomFade).toFloat ();

			juce::ColourGradient fade (theme::card ().withAlpha (0.0f), bottom.getX (), bottom.getY (),
									   theme::card (), bottom.getX (), bottom.getBottom (), false);
			g.setGradientFill (fade);
			g.fillRect (bottom);
		}
	}

	void resized (void) override
	{
		exportButton.setBounds (getWidth () - kInset - 112, 10, 112, 24);

		// Tail to tail with the card, above the fade at the bottom: a thumb that ran into the fade
		// would look like part of it.
		// Inside the card's right edge, in the column every row reserves for it.
		clampScroll ();
		relayout ();
		verticalBar.setBounds (scrollBarTrack ());
		syncScrollBar ();
		repaint ();
	}

	/** Everything that changes the bar. Called from the load path and from user input, never from
		paint(): a child whose visibility is toggled during a paint callback has that paint
		cancelled for the frame, which is what made the first attempt at this bar invisible while
		the component tree said it was there.
	*/
	void syncScrollBar (void)
	{
		const int visible = juce::jmax (1, getHeight () - kHeader);

		verticalBar.setVisible (contentHeight > visible);

		if (contentHeight > visible)
		{
			verticalBar.setRangeLimits (0.0, (double) contentHeight, juce::dontSendNotification);
			verticalBar.setCurrentRange ((double) scrollOffset, (double) visible,
										 juce::dontSendNotification);
		}
	}

	void scrollBarMoved (juce::ScrollBar* bar, double newRangeStart) override
	{
		if (bar != &verticalBar)
		{
			return;
		}

		scrollOffset = (int) std::lround (newRangeStart);
		clampScroll ();
		relayout ();
		repaint ();
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
		// The rows are built in relayout, which runs when the panel paints. A pointer that arrives
		// before the first paint - a script, a restored window - would be hit-tested against an
		// empty list.
		if (rows.empty ())
		{
			relayout ();
		}

		const auto point = event.getPosition ();
		const int hovered = shiftLeftRect.contains (point) ? 0
						  : (shiftRightRect.contains (point) ? 1 : -1);

		int rowChannel = -1;
		bool rowMute = false;

		if (hovered < 0)
		{
			for (const auto& row : rows)
			{
				if (row.header)
				{
					continue;
				}

				if (row.mute.contains (point)) { rowChannel = row.channel; rowMute = true;  break; }
				if (row.solo.contains (point)) { rowChannel = row.channel; rowMute = false; break; }
			}
		}


		const bool pointer = hovered >= 0 || rowChannel >= 0;

		if (pointer != lastPointerWasOverControl)
		{
			lastPointerWasOverControl = pointer;
			setMouseCursor (pointer ? juce::MouseCursor::PointingHandCursor
									: juce::MouseCursor::NormalCursor);
		}

		if (hovered != hoveredControl)
		{
			hoveredControl = hovered;
			repaint (getLocalBounds ().withHeight (kHeader));
		}

		if (rowChannel != hoveredRowChannel || rowMute != hoveredRowMute)
		{
			// Only the two rows involved: repainting thirteen of them at every mouse move is how a
			// hover effect becomes a fan.
			const auto repaintRow = [this] (int channel)
			{
				for (const auto& row : rows)
				{
					if (! row.header && row.channel == channel)
					{
						repaint (row.mute.getUnion (row.solo).expanded (2));
					}
				}
			};

			if (hoveredRowChannel >= 0) { repaintRow (hoveredRowChannel); }

			hoveredRowChannel = rowChannel;
			hoveredRowMute = rowMute;

			if (hoveredRowChannel >= 0) { repaintRow (hoveredRowChannel); }
		}
	}

	void mouseExit (const juce::MouseEvent&) override
	{
		if (hoveredRowChannel >= 0)
		{
			hoveredRowChannel = -1;
			repaint ();

			lastPointerWasOverControl = false;
			setMouseCursor (juce::MouseCursor::NormalCursor);
		}

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
		scrollOffset -= (int) std::lround (wheel.deltaY * (float) metrics.rowHeight * 3.0f);
		clampScroll ();
		relayout ();
		syncScrollBar ();
		repaint ();
	}

	std::function<void ()> onExport;

private:
	using Row = keyboard::Row;

	/** Rebuilds the rows and the content height from the width, the snapshot and the scroll.

		Kept out of paint(): it shows and hides the scroll bar, and toggling a child's visibility
		from inside a paint callback cancels that child's paint for the frame - the bar was visible
		according to the component tree and drew nothing at all.
	*/
	void relayout (void)
	{
		// The column is the card's width minus the bar, always: a bar that appeared and vanished
		// with the content would reflow every key each time a song gained a channel.
		metrics.reservedRight = kScrollBarWidth + 2 * kScrollBarGap;

		const auto layout = keyboard::layoutRows (chipNamesFor (snapshot), getWidth (),
												 scrollOffset, kHeader, metrics);

		rows = std::move (layout.rows);
		contentHeight = layout.contentHeight;
	}

	/** Pushes the content height into the bar without the bar pushing it back. */

	/** The card's scroll bar, drawn rather than built.

		A juce::ScrollBar is a child component, and as a child of this panel it could not be made to
		paint: visible in the component tree, bounds inside the card, and not one coloured pixel on
		screen. Ten lines of drawing take the palette's colours like everything else and cannot end
		up behind the rows.
	*/
	juce::Rectangle<int> scrollBarTrack (void) const
	{
		return { getWidth () - metrics.cardInset - kScrollBarGap - kScrollBarWidth,
				 kHeader + kScrollBarGap,
				 kScrollBarWidth,
				 juce::jmax (0, getHeight () - kHeader - kBottomFade) };
	}

	void clampScroll (void)
	{
		scrollOffset = keyboard::clampScroll (scrollOffset, contentHeight,
											  juce::jmax (0, getHeight () - kHeader));
	}

	/** The chips in snapshot order - everything layoutRows needs to know about grouping. */
	static std::vector<std::string> chipNamesFor (const toybox::playback::ChipStateSnapshot& from)
	{
		std::vector<std::string> names;
		names.reserve (from.channels.size ());

		for (const auto& channel : from.channels)
		{
			names.push_back (channel.chipName);
		}

		return names;
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

	/** Mute and solo, drawn with the mixer's own face and colours: mute fills red, solo amber,
		and a toggled one carries the matching glow. Anything else here would make the two views
		look like different applications. */
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
		const bool canGoDown = canShift (-1);
		const bool canGoUp = canShift (1);

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
		g.drawText (noteName (base, octaveShiftForNaming ()), autoRect, juce::Justification::centred, false);
	}



	/** Keys are a fixed width. A wider window shows more of them rather than bigger ones, which
		is what a keyboard is: the distance between two keys does not change because the window
		did. */
	static constexpr int kWhiteKeyWidth = 20;
	static constexpr int kMinWhiteKeyWidth = 16;
	// The whole MIDI range: note 0 to 127. What those are called depends on where middle C is
	// named, which is a setting - the range itself is not.
	static constexpr int kFirstMusicalNote = 0;
	static constexpr int kLastMusicalNote = 127;

	using KeySpan = keyboard::KeySpan;

	// Thin forwarders: the panel keeps calling these by name, the arithmetic lives in KeyboardLayout.
	static int octaveFloor (int note) { return keyboard::octaveFloor (note); }
	static int noteForWhite (int base, int k) { return keyboard::noteForWhite (base, k); }

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

	/** Which notes this row shows: the arithmetic is in KeyboardLayout, the starting point is here
		because it depends on the song, the setting and the user's last shift. */
	KeySpan spanFor (juce::Rectangle<int> area) const
	{
		const int width = area.getWidth ();
		return keyboard::spanFor (width, baseNote (keyboard::octavesFor (width)));
	}

	bool canShift (int direction) const
	{
		return keyboard::canShift (lastSpan, direction);
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

		const int octaveShift = octaveShiftForNaming ();

		const float whiteWidth = (float) span.keyWidth;
		const float blackWidth = whiteWidth * 0.62f;
		const float blackHeight = (float) area.getHeight () * 0.62f;

		// The bed is exactly as wide as the keys that are drawn, and centred in the column. A bed
		// that filled the column left a bare strip on the right whenever the keys stopped at B8 -
		// pale, empty, and the first thing the eye went to.
		const float keysWidth = (float) span.whiteKeys * whiteWidth;
		const float offsetX = (float) area.getX ()
							  + juce::jmax (0.0f, ((float) area.getWidth () - keysWidth) * 0.5f);
		const juce::Rectangle<float> bed (offsetX, (float) area.getY (),
										  juce::jmin (keysWidth, (float) area.getWidth ()),
										  (float) area.getHeight ());

		drawSoftShadow (g, bed, 5.0f, theme::cardKeyShadow (), 5, juce::Point<int> (0, 1));

		g.setColour (theme::keyBed ());
		g.fillRoundedRectangle (bed, 5.0f);

		{
			juce::Graphics::ScopedSaveState inner (g);
			g.reduceClipRegion (bed.toNearestInt ());

			juce::ColourGradient shade (theme::shadowTint ().withAlpha (0.10f), bed.getX (), bed.getY (),
										juce::Colours::transparentBlack, bed.getX (), bed.getY () + 5.0f, false);
			g.setGradientFill (shade);
			g.fillRoundedRectangle (bed, 5.0f);
		}

		g.setColour (theme::cardBorder ().withAlpha (0.7f));
		g.drawRoundedRectangle (bed.reduced (0.5f), 5.0f, 1.0f);

		// The keys take their colours from the palette; nothing here mixes card and text, because a
		// ratio that reads well on the light card is mud on the dark one.
		const juce::Colour whiteKey = theme::keyWhite ();
		const juce::Colour blackKey = theme::keyBlack ();
		const juce::Colour litKey = theme::accent ();
		const juce::Colour litBlack = theme::accent ().darker (0.22f);

		const auto drawKey = [&] (int keyNote, juce::Rectangle<float> key, bool black)
		{
			const bool lit = keyNote == note;

			// A press moves the key down a pixel and darkens it, so the finger has somewhere to
			// have gone. The key is drawn one pixel taller to keep its top edge where it was.
			const juce::Rectangle<float> face = lit ? key.translated (0.0f, 1.0f) : key;

			g.setColour (lit ? (black ? litBlack : litKey) : (black ? blackKey : whiteKey));
			g.fillRoundedRectangle (face, 1.5f);

			if (lit)
			{
				// an inner shadow along the top: what makes it read as pressed rather than tinted
				juce::Graphics::ScopedSaveState clipped (g);
				g.reduceClipRegion (face.toNearestInt ());

				juce::ColourGradient shade (theme::shadowTint ().withAlpha (0.30f), face.getX (), face.getY (),
											juce::Colours::transparentBlack, face.getX (), face.getY () + 5.0f, false);
				g.setGradientFill (shade);
				g.fillRoundedRectangle (face, 1.5f);

				g.setColour (theme::onAccent ().withAlpha (0.55f));
				g.fillRect (juce::Rectangle<float> (face.getX () + 1.0f, face.getBottom () - 1.5f,
													face.getWidth () - 2.0f, 1.0f));
			}

			if (lit)
			{
				// The note this key is playing sits on a disc of its own rather than being painted
				// straight onto the key. The key is already the accent colour, so the label cannot
				// be - a disc in the palette's on-accent colour with the accent in it keeps the two
				// apart on the light and the dark palette alike, and the shadow lifts it off the
				// key the way the octave arrows sit above theirs.
				//
				// White keys get the disc in the lower third (the black key above covers the top),
				// black keys in the middle. Both are sized to the key, so a narrow black key gets a
				// smaller disc and a squeezed font rather than a label spilling onto its neighbours.
				const float diameter = juce::jmin (face.getWidth () - 1.0f, black ? 11.0f : 15.0f);

				if (diameter >= 8.0f)
				{
					const auto centre = juce::Point<float> (face.getCentreX (),
															black ? face.getCentreY ()
																  : face.getY () + face.getHeight () * 0.68f);
					const auto disc = juce::Rectangle<float> (diameter, diameter).withCentre (centre);

					juce::Path discPath;
					discPath.addEllipse (disc);

					juce::DropShadow (theme::shadowTint ().withAlpha (0.32f), 2, { 0, 1 })
						.drawForPath (g, discPath);

					// Same disc as the off-keyboard label, and for the same reason: white on the
					// light palette, a mid grey on the dark one, never the on-accent black.
					g.setColour (theme::keyWhite ());
					g.fillEllipse (disc);

					g.setColour (theme::cardBorder ().withAlpha (0.45f));
					g.drawEllipse (disc.reduced (0.5f), 1.0f);

					const float size = juce::jlimit (7.0f, 11.0f, diameter * 0.74f);
					juce::Font font (juce::FontOptions (size, juce::Font::bold));
					font.setHorizontalScale (black ? 0.82f : 1.0f);

					g.setColour (theme::accent ());
					g.setFont (font);
					g.drawText (noteName (keyNote, octaveShift), disc.toNearestInt (),
								juce::Justification::centred, false);
				}
			}
		};

		// The gap between two white keys, drawn rather than left to the bed showing through: at
		// twenty pixels wide and one pixel of gap, the bed was too thin to read as a separation,
		// so the keys ran together into one pale strip.
		const auto drawKeyGap = [&] (juce::Rectangle<float> key)
		{
			g.setColour (theme::track ());
			g.fillRect (juce::Rectangle<float> (key.getRight (), key.getY () + 1.0f,
												1.0f, key.getHeight () - 2.0f));
		};

		// White keys first, then the blacks on top of them.
		for (int k = 0; k < span.whiteKeys; ++k)
		{
			const int keyNote = noteForWhite (span.base, k);

			// The white-key count is an estimate rounded up to a whole octave; past G8 there is
			// nothing to draw, so the last one or two of them are dropped here.
			if (keyNote > kLastMusicalNote)
			{
				continue;
			}

			const juce::Rectangle<float> key (offsetX + whiteWidth * (float) k,
											  (float) area.getY (), whiteWidth - 1.0f,
											  (float) area.getHeight ());
			drawKey (keyNote, key, false);
			drawKeyGap (key);
		}

		for (int k = 0; k < span.whiteKeys; ++k)
		{
			const int pc = k % 7;

			// E and B have no black key above them; the rest do.
			if (pc == 2 || pc == 6) { continue; }

			const int keyNote = noteForWhite (span.base, k) + 1;

			if (keyNote > kLastMusicalNote) { continue; }

			const juce::Rectangle<float> key (offsetX + whiteWidth * (float) (k + 1) - blackWidth * 0.5f,
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

				if (keyNote > kLastMusicalNote)
				{
					break;
				}

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

				const juce::Rectangle<float> key (offsetX + whiteWidth * (float) k + 1.0f,
												  (float) area.getY (), whiteWidth - 2.0f,
												  (float) area.getHeight ());

				g.setColour (theme::secondaryText ().withAlpha (0.75f));
				g.drawText (noteName (keyNote, octaveShift), key.toNearestInt ().withTrimmedBottom (1),
							juce::Justification::centredBottom, false);
			}
		}

		// A fade at an end that still has keys beyond it. The range stops at B8 by arithmetic, and
		// the window can be narrower than the music; either way the edge is the only place that can
		// say "there is more this way" without moving anything. Drawn inside the bed.
		{
			juce::Graphics::ScopedSaveState clipped (g);
			g.reduceClipRegion (bed.toNearestInt ());

			if (span.base > kFirstMusicalNote)
			{
				juce::ColourGradient fade (theme::edgeFade (), bed.getX (), bed.getY (),
										   juce::Colours::transparentBlack, bed.getX () + 12.0f, bed.getY (), false);
				g.setGradientFill (fade);
				g.fillRect (bed.withWidth (12.0f));
			}

			if (span.lastNote < kLastMusicalNote - 12)
			{
				juce::ColourGradient fade (juce::Colours::transparentBlack, bed.getRight () - 12.0f, bed.getY (),
										   theme::edgeFade (), bed.getRight (), bed.getY (), false);
				g.setGradientFill (fade);
				g.fillRect (bed.withTrimmedLeft (bed.getWidth () - 12.0f));
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

			// A blurred halo behind the arrow: it sits on top of the keys, and a hard triangle on a
			// keyboard reads as a stray mark. The blur is the same helper the cards use.
			drawSoftShadow (g, juce::Rectangle<float> (cx - 7.0f, cy - 7.0f, 14.0f, 14.0f), 7.0f,
							theme::accentGlow (), 6, juce::Point<int> (0, 0));

			g.setColour (theme::accent ().withAlpha (0.9f));
			g.fillPath (arrow);

			// The note that is off the edge, named on a disc in the middle of the keyboard. It is
			// the one thing in this row that is not a key, and painting it straight onto the keys
			// read as small print on whichever key happened to be under it - the accent disc says
			// "this note is somewhere else" the same way the arrows do.
			const auto label = noteName (note, octaveShift);
			const float diameter = juce::jmax (20.0f, (float) label.length () * 6.6f + 9.0f);

			const auto disc = juce::Rectangle<float> (diameter, diameter)
								  .withCentre ({ (float) area.getCentreX (), (float) area.getCentreY () });

			// The same halo the arrows use: both sit on top of the keys, and a hard shape there
			// reads as a stray mark rather than as something floating above them.
			drawSoftShadow (g, disc.expanded (1.0f), 6.0f, theme::accentGlow (), 6, juce::Point<int> (0, 0));

			// White on the light palette, the white key's own colour, and a mid grey on the dark
			// one. The on-accent colour this replaces is pure black there, which reads as a hole
			// punched through the keys; a hairline border below keeps the disc's edge where it
			// lands on a same-coloured key.
			g.setColour (theme::keyWhite ());
			g.fillEllipse (disc);

			g.setColour (theme::cardBorder ().withAlpha (0.45f));
			g.drawEllipse (disc.reduced (0.5f), 1.0f);

			g.setColour (theme::accent ());
			g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
			g.drawText (label, disc.toNearestInt (), juce::Justification::centred, false);
		}
	}

	/** Moves the window by whole octaves; the setting and the song's observed range both yield
		to it until the next song. */
	void shiftOctaves (int direction)
	{
		if (! canShift (direction))
		{
			return;
		}

		const int step = direction > 0 ? 12 : -12;
		manualBase = juce::jlimit (kFirstMusicalNote, kLastMusicalNote - 12, lastSpan.base + step);
		repaint ();
	}

	/** The note's own level - velocity, not loudness.

		This is a different quantity from the mixer's meters, which show what is coming out of the
		speakers and turn amber and red as it approaches full scale. A channel's velocity says how
		hard the note is played, and a pulse played at full volume is the normal case rather than a
		warning - so it gets the accent colour, the same one the lit key uses, and no threshold.
		Painting it red at the top of the scale made every square-wave song look like it was about
		to clip.
	*/
	toybox::PlaybackEngine& engine;
	toybox::playback::ChipStateSnapshot snapshot;
	std::vector<Row> rows;

	/** One waveform path per channel, kept across frames - see WavePathCache. Grown to the channel
		count when a snapshot arrives, never shrunk: the entries cost a Path each, not a picture. */
	std::vector<keyboard::WavePathCache> waveCaches;


	int scrollOffset = 0;
	int contentHeight = 0;

	/** The real thing, so its hover and drag come from the same look and feel the mixer's bar
		uses. Its bounds are set in resized(); its visibility and range in syncScrollBar() - both
		outside paint(), and that is the whole of the difference from the version that refused to
		appear. */
	juce::ScrollBar verticalBar { true };

	// What each channel last played, and when - the note-off delay reads from here.
	struct Held
	{
		int note = -1;
		float volume = 0.0f;
		double at = 0.0;
	};

	std::vector<Held> held;

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

	// Which row's mute or solo the pointer is over: -1 for none. The two are one member because
	// only one of them can be under the pointer at a time.
	int hoveredRowChannel = -1;
	bool hoveredRowMute = false;
	bool lastPointerWasOverControl = false;

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

	// isVisible(), not isShowing(): a component in a window has both, but the headless harness
	// builds the editor without a peer - showing is then false forever, and the panel stayed empty
	// in every screenshot the harness took. Being invisible still closes the gate, which is the
	// case that matters (the view is not the one on screen).
	if (! isVisible ())
	{
		engine.setStateCaptureWanted (false);
		return;
	}

	engine.setStateCaptureWanted (true);

	// Three outcomes, and the panel needs two of them. A capture that arrives is drawn; a frame
	// that lost the lock leaves the last one alone, because a dropped frame is not a change of
	// song; and a song with no chip state at all arrives as a snapshot whose valid flag is off -
	// which has to be handed on, or the keyboard keeps painting the song before it.
	const bool captured = engine.captureChipState (snapshot);

	if (! captured && snapshot.valid)
	{
		return;
	}

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
