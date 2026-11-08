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

	songBox.setComponentID ("song.selector");
	songBox.onChange = [this] { switchToSelectedSong (); };
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
	}

	titleLabel.setBounds (titleRow);
	detailsLabel.setBounds (inner.removeFromTop (20));
	fileLabel.setBounds (inner.removeFromTop (18));
}

void SongInfoPanel::refresh (void)
{
	if (! engine.isLoaded())
	{
		songBox.setVisible (false);
		titleLabel.setText (i18n::tr ("song.noFile"), juce::dontSendNotification);
		detailsLabel.setText ({}, juce::dontSendNotification);
		fileLabel.setText (formatLoadHint(),
						   juce::dontSendNotification);
		return;
	}

	// **One entry per song, and nothing at all for a single-song format.** The list is rebuilt only when
	// the count changes, so a playing song does not reset the user's open popup on every tick.
	const int songCount = engine.getSongCount();

	if (songBox.isVisible () != (songCount > 1))
	{
		songBox.setVisible (songCount > 1);
		resized ();
	}

	if (songCount > 1)
	{
		if (songBox.getNumItems () != songCount)
		{
			songBox.clear (juce::dontSendNotification);

			for (int index = 1; index <= songCount; ++index)
			{
				songBox.addItem (juce::String (index), index);
			}
		}

		songBox.setSelectedId (engine.getSongIndex () + 1, juce::dontSendNotification);
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

	// A failed switch leaves the list where it was, so the control never claims a song that is not open.
	juce::String error;

	if (! engine.selectSong (index, error))
	{
		songBox.setSelectedId (engine.getSongIndex () + 1, juce::dontSendNotification);
	}
}

} // namespace toybox::ui
