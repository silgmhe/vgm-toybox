#include "SongInfoPanel.h"

#include "Format.h"
#include "Theme.h"

#include "i18n/LanguageManager.h"
#include "playback/BackendRegistry.h"
#include "playback/PlaybackEngine.h"

namespace toybox::ui
{

SongInfoPanel::SongInfoPanel (toybox::PlaybackEngine& engineToUse)
	: engine (engineToUse)
{
	titleLabel.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
	titleLabel.setJustificationType (juce::Justification::centredLeft);
	addAndMakeVisible (titleLabel);

	detailsLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
	detailsLabel.setJustificationType (juce::Justification::centredLeft);
	addAndMakeVisible (detailsLabel);

	fileLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
	// all three labels exist by now: apply their colours, and re-apply them on a theme change
	lookAndFeelChanged();
	fileLabel.setJustificationType (juce::Justification::centredLeft);
	addAndMakeVisible (fileLabel);

	songLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
	songLabel.setJustificationType (juce::Justification::centredRight);
	songLabel.setComponentID ("song.label");
	addChildComponent (songLabel);

	songBox.setComponentID ("song.selector");
	songBox.onChange = [this] { switchToSelectedSong (); };
	// A switch builds in the background, and `getSongIndex()` keeps reporting the old song until it
	// lands - no grace window covers that, so the box is held off outright while it runs.
	songBox.holdOff = [this] { return switchingSong; };
	addChildComponent (songBox);		// refresh() shows it only for a file with more than one song

	refresh();
}

void SongInfoPanel::lookAndFeelChanged (void)
{
	// the song title is the loudest text in the card, the details and the file name are the quiet
	// ones
	titleLabel.setColour (juce::Label::textColourId, theme::primaryText());
	detailsLabel.setColour (juce::Label::textColourId, theme::secondaryText());
	fileLabel.setColour (juce::Label::textColourId, theme::secondaryText());
	songLabel.setColour (juce::Label::textColourId, theme::secondaryText());
}

void SongInfoPanel::refreshLanguage (void)
{
	refresh();
}

void SongInfoPanel::resized (void)
{
	auto inner = getLocalBounds().reduced (theme::cardPadding, 10);
	auto titleRow = inner.removeFromTop (26);

	// The selector sits at the end of the title row and takes nothing from the text when it is hidden.
	if (songBox.isVisible ())
	{
		songBox.setBounds (titleRow.removeFromRight (96).reduced (0, 2));
		songLabel.setBounds (titleRow.removeFromRight (46).reduced (0, 2));
	}

	titleLabel.setBounds (titleRow);
	detailsLabel.setBounds (inner.removeFromTop (20));
	fileLabel.setBounds (inner.removeFromTop (18));
}

void SongInfoPanel::refresh (void)
{
	if (! engine.isLoaded())
	{
		if (songBox.isVisible () || songLabel.isVisible ())
		{
			// Relayout as well, or the title keeps the width the selector was reserving for it.
			songBox.setVisible (false);
			songLabel.setVisible (false);
			resized ();
		}

		titleLabel.setText (i18n::tr ("song.noFile"), juce::dontSendNotification);
		detailsLabel.setText ({}, juce::dontSendNotification);
		fileLabel.setText (formatLoadHint(),
						   juce::dontSendNotification);
		return;
	}

	// **One entry per song, and nothing at all for a single-song format.** The list is rebuilt only when
	// the count changes, so a playing song does not reset the user's open popup on every tick.
	const int songCount = engine.getSongCount();

	const bool wantsSelector = songCount > 1;

	if (songBox.isVisible () != wantsSelector)
	{
		songBox.setVisible (wantsSelector);
		songLabel.setVisible (wantsSelector);
		resized ();
	}

	if (wantsSelector)
	{
		// Re-read every tick: a language change has to reach this label like every other one.
		songLabel.setText (i18n::tr ("song.track"), juce::dontSendNotification);
		if (songBox.getNumItems () != songCount)
		{
			songBox.clear (juce::dontSendNotification);

			for (int index = 1; index <= songCount; ++index)
			{
				songBox.addItem (juce::String (index), index);
			}

			// A cleared box holds no selection at all, and `syncTo` could leave it that way for the
			// length of a pick's grace window - a rebuilt list always ends pinned.
			songBox.forceTo (engine.getSongIndex () + 1);
		}
		else
		{
			// Not an unconditional write any more: that is what used to undo a pick before its
			// notification had been delivered (SyncedComboBox.h has the whole story).
			songBox.syncTo (engine.getSongIndex () + 1);
		}
	}

	const auto& file = engine.getFile();
	juce::String title = engine.getTag ("TITLE");
	if (title.isEmpty())
	{
		title = file.getFileNameWithoutExtension();
	}
	titleLabel.setText (title, juce::dontSendNotification);

	juce::String details;
	const auto appendTag = [&details] (const juce::String& name, const juce::String& value)
	{
		if (value.isNotEmpty())
		{
			details << name << ": " << value << "    ";
		}
	};
	appendTag (i18n::tr ("tag.game"), engine.getTag ("GAME"));
	appendTag (i18n::tr ("tag.system"), engine.getTag ("SYSTEM"));
	appendTag (i18n::tr ("tag.artist"), engine.getTag ("ARTIST"));
	appendTag (i18n::tr ("tag.date"), engine.getTag ("DATE"));
	appendTag (i18n::tr ("tag.copyright"), engine.getTag ("COPYRIGHT"));
	detailsLabel.setText (details.trim(), juce::dontSendNotification);

	fileLabel.setText (file.getFileName() + "   -   " + formatTime (engine.getPositionSeconds())
						   + " / " + formatTime (engine.getDurationSeconds()),
					   juce::dontSendNotification);
}

void SongInfoPanel::switchToSelectedSong (void)
{
	if (! engine.isLoaded ())
	{
		return;
	}

	const int index = songBox.getSelectedId () - 1;

	if (index < 0 || index == engine.getSongIndex ())
	{
		return;
	}

	// **Built on the load worker**, because a rebuild costs what a file open costs and the window has to
	// keep drawing through it. The selection is set optimistically so the box shows the song being
	// built, and `switchingSong` holds the refresh timer off the box until the build lands - the
	// engine still reports the *old* song throughout, so any mirroring before then would silently
	// put the previous choice back (that is the whole of the "song selector does nothing" report).
	switchingSong = true;
	songBox.forceTo (index + 1);

	juce::Component::SafePointer<SongInfoPanel> safe (this);

	const bool accepted = engine.selectSongAsync (index, [safe] (bool ok, const juce::String& error)
	{
		if (safe == nullptr)
		{
			return;
		}

		safe->switchingSong = false;

		if (ok)
		{
			return;		// the box already shows the song that has just been installed
		}

		// Refused or failed: put the box back on the song that is actually open. Pinned rather than
		// mirrored, because the pick's grace window is still open and would suppress the mirror.
		safe->songBox.forceTo (safe->engine.getSongIndex () + 1);

		if (safe->onSwitchFailure != nullptr)
		{
			safe->onSwitchFailure (error);
		}
	});

	if (! accepted)
	{
		switchingSong = false;
		songBox.forceTo (engine.getSongIndex () + 1);
	}
}

} // namespace toybox::ui
