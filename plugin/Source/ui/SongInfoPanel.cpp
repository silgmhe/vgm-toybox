#include "SongInfoPanel.h"

#include "Format.h"
#include "Theme.h"

#include "vgm/MixerEngine.h"

namespace vgmplay::ui
{

SongInfoPanel::SongInfoPanel (const vgmplay::MixerEngine& engineToUse)
	: engine (engineToUse)
{
	titleLabel.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
	titleLabel.setColour (juce::Label::textColourId, theme::primaryText());
	titleLabel.setJustificationType (juce::Justification::centredLeft);
	addAndMakeVisible (titleLabel);

	detailsLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
	detailsLabel.setColour (juce::Label::textColourId, theme::secondaryText());
	detailsLabel.setJustificationType (juce::Justification::centredLeft);
	addAndMakeVisible (detailsLabel);

	fileLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
	fileLabel.setColour (juce::Label::textColourId, theme::secondaryText());
	fileLabel.setJustificationType (juce::Justification::centredLeft);
	addAndMakeVisible (fileLabel);

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
		titleLabel.setText ("No file loaded", juce::dontSendNotification);
		detailsLabel.setText ({}, juce::dontSendNotification);
		fileLabel.setText ("Load a .vgm / .vgz file with the button above or drop one onto this window.",
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
	appendTag ("Game", engine.getTag ("GAME"));
	appendTag ("System", engine.getTag ("SYSTEM"));
	appendTag ("Artist", engine.getTag ("ARTIST"));
	appendTag ("Date", engine.getTag ("DATE"));
	detailsLabel.setText (details.trim(), juce::dontSendNotification);

	fileLabel.setText (file.getFileName() + "   -   " + formatTime (engine.getPositionSeconds())
						   + " / " + formatTime (engine.getDurationSeconds()),
					   juce::dontSendNotification);
}

} // namespace vgmplay::ui
