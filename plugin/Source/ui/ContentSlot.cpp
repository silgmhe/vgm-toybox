#include "ContentSlot.h"

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

	void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
	{
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

	void paintToggle (juce::Graphics& g, juce::Rectangle<int> area, const char* letter, bool on, bool warn)
	{
		g.setColour (on ? (warn ? theme::accent ().withAlpha (0.9f) : theme::accent ().withAlpha (0.75f))
						: theme::cardBorder ());
		g.fillRoundedRectangle (area.toFloat (), 3.0f);

		g.setColour (on ? theme::onAccent () : theme::secondaryText ());
		g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
		g.drawText (letter, area, juce::Justification::centred, false);
	}

	/** One cycle, stepped. A table is drawn as it is; a generated shape is drawn from the
		registers that define it - which is the same thing the chip does, one level up. */
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

	/** How many octaves fit. Twenty pixels per white key is the smallest that still reads as a
		keyboard; below that the range shrinks rather than the keys. */
	int octavesFor (int keysWidth) const
	{
		return juce::jlimit (3, 7, keysWidth / (7 * 20));
	}

	/** Four octaves at a narrow window, seven at a wide one, the left edge pinned to C2.

		Pinned on purpose: a keyboard centred on C4 would slide every key on every resize, and
		thirteen rows sliding independently reads as a fault rather than as a layout.
	*/
	void paintKeyboard (juce::Graphics& g, juce::Rectangle<int> area, int note)
	{
		if (area.getWidth () < 80 || area.getHeight () < 12)
		{
			return;
		}

		constexpr int firstNote = 36;		// C2
		constexpr int whiteIndex[12] = { 0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6 };

		const int octaves = octavesFor (area.getWidth ());
		const int lastNote = firstNote + octaves * 12;

		const float whiteWidth = (float) area.getWidth () / (float) (octaves * 7);
		const float blackWidth = whiteWidth * 0.62f;
		const float blackHeight = (float) area.getHeight () * 0.62f;

		// Opaque colours. A translucent black key let the white keys' divider lines show through
		// it, which looked like a printing fault rather than like a keyboard.
		const juce::Colour whiteKey = theme::card ().interpolatedWith (theme::primaryText (), 0.09f);
		const juce::Colour blackKey = theme::card ().interpolatedWith (theme::primaryText (), 0.74f);
		const juce::Colour litKey = theme::accent ();
		const juce::Colour litBlack = theme::accent ().darker (0.25f);

		const auto keyFor = [&] (int offset, float& x, float& width, bool& black)
		{
			const int octave = offset / 12;
			const int pitchClass = offset % 12;

			if (whiteIndex[pitchClass] >= 0)
			{
				x = (float) area.getX () + whiteWidth * (float) (octave * 7 + whiteIndex[pitchClass]);
				width = whiteWidth;
				black = false;
			}
			else
			{
				x = (float) area.getX () + whiteWidth * (float) (octave * 7 + whiteIndex[pitchClass - 1] + 1)
					- blackWidth * 0.5f;
				width = blackWidth;
				black = true;
			}
		};

		const auto paintKey = [&] (int offset, bool wantBlack)
		{
			float x = 0.0f, width = 0.0f;
			bool black = false;
			keyFor (offset, x, width, black);

			if (black != wantBlack)
			{
				return;
			}

			const int keyNote = firstNote + offset;
			const bool lit = note == keyNote;
			const float height = black ? blackHeight : (float) area.getHeight ();
			const juce::Rectangle<float> key (x + 0.5f, (float) area.getY (), width - 1.0f, height);

			g.setColour (lit ? (black ? litBlack : litKey) : (black ? blackKey : whiteKey));
			g.fillRoundedRectangle (key, 1.5f);

			if (lit && width >= 13.0f)
			{
				g.setColour (theme::onAccent ());
				g.setFont (juce::Font (juce::FontOptions (9.0f)));
				g.drawText (noteName (keyNote), key.toNearestInt (), juce::Justification::centred, false);
			}
		};

		for (int octave = 0; octave < octaves; ++octave)
		{
			for (int pc = 0; pc < 12; ++pc)
			{
				if (whiteIndex[pc] >= 0)
				{
					paintKey (octave * 12 + pc, false);
				}
			}
		}

		for (int octave = 0; octave < octaves; ++octave)
		{
			for (int pc = 0; pc < 12; ++pc)
			{
				if (whiteIndex[pc] < 0)
				{
					paintKey (octave * 12 + pc, true);
				}
			}
		}

		// The octave names, on the C keys only. Three density steps, because a label that collides
		// with its neighbour is worse than no label at all.
		const int everyNth = whiteWidth >= 15.0f ? 1 : (whiteWidth >= 10.0f ? 2 : 0);

		if (everyNth > 0)
		{
			g.setFont (juce::Font (juce::FontOptions (8.5f)));

			for (int octave = 0; octave < octaves; octave += everyNth)
			{
				const int keyNote = firstNote + octave * 12;

				if (keyNote == note)
				{
					continue;		// the row's own note label already names this key
				}

				const float x = (float) area.getX () + whiteWidth * (float) (octave * 7);
				const juce::Rectangle<float> key (x + 1.0f, (float) area.getY (),
												  whiteWidth - 2.0f, (float) area.getHeight ());

				g.setColour (theme::secondaryText ().withAlpha (0.75f));
				g.drawText (noteName (keyNote), key.toNearestInt ().withTrimmedBottom (1),
							juce::Justification::centredBottom, false);
			}
		}

		// Out of range: a narrow window shows fewer octaves, and a note above or below them has
		// nowhere to light. The arrow says which way it went without moving the keyboard.
		if (note >= 0 && (note < firstNote || note >= lastNote)
			&& toybox::settings::SettingsStore::getInstance ().getKeyboardRangeHints ())
		{
			const bool above = note >= lastNote;
			const float size = 4.0f;
			const float cx = above ? (float) area.getRight () - 6.0f : (float) area.getX () + 6.0f;
			const float cy = (float) area.getCentreY ();

			juce::Path arrow;

			if (above)
			{
				arrow.addTriangle (cx - size, cy + size, cx + size, cy + size, cx, cy - size);
			}
			else
			{
				arrow.addTriangle (cx - size, cy - size, cx + size, cy - size, cx, cy + size);
			}

			g.setColour (theme::accent ().withAlpha (0.9f));
			g.fillPath (arrow);

			g.setColour (theme::secondaryText ());
			g.setFont (juce::Font (juce::FontOptions (8.5f)));
			g.drawText (noteName (note), area.withTrimmedLeft (above ? 0 : 12).withTrimmedRight (above ? 12 : 0),
						juce::Justification::centred, false);
		}
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
