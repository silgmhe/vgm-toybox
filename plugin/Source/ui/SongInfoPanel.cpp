#include "SongInfoPanel.h"

#include "Format.h"
#include "Theme.h"

#include "i18n/LanguageManager.h"
#include "playback/BackendRegistry.h"
#include "playback/PlaybackEngine.h"

namespace toybox::ui
{

SongInfoPanel::SongInfoPanel (const toybox::PlaybackEngine& engineToUse)
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
	titleLabel.setBounds (inner.removeFromTop (26));
	detailsLabel.setBounds (inner.removeFromTop (20));
	fileLabel.setBounds (inner.removeFromTop (18));
}

void SongInfoPanel::refresh (void)
{
	if (! engine.isLoaded())
	{
		titleLabel.setText (i18n::tr ("song.noFile"), juce::dontSendNotification);
		detailsLabel.setText ({}, juce::dontSendNotification);
		fileLabel.setText (formatLoadHint(),
						   juce::dontSendNotification);
		return;
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
	detailsLabel.setText (details.trim(), juce::dontSendNotification);

	fileLabel.setText (file.getFileName() + "   -   " + formatTime (engine.getPositionSeconds())
						   + " / " + formatTime (engine.getDurationSeconds()),
					   juce::dontSendNotification);
}

} // namespace toybox::ui
