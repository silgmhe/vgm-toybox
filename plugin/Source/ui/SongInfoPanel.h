#pragma once

#include "CardComponent.h"

namespace toybox { class PlaybackEngine; }

namespace toybox::ui
{

/**
	The "song info" card: title, the most important tags and the file name with the play
	position. Everything is pulled from the engine in refresh(), so the editor only has to
	call that once per timer tick - there is no state pushed in from the outside.
*/
class SongInfoPanel : public CardComponent
{
public:
	static constexpr int preferredHeight = 96;
	static constexpr int minHeight = 70;

	explicit SongInfoPanel (toybox::PlaybackEngine& engineToUse);

	/** Updates title, tags and the file/position line. */
	void refresh (void);

	/** Re-reads the labels that come from the language pack. */
	void refreshLanguage (void);

	/** Re-applies the label colours (see PluginEditor::lookAndFeelChanged for why this is
		needed at all). */
	void lookAndFeelChanged (void) override;

private:
	void resized (void) override;

	toybox::PlaybackEngine& engine;

	juce::Label titleLabel;
	juce::Label detailsLabel;
	juce::Label fileLabel;

	/** **The song list of a multi-song file.** Hidden unless the file holds more than one, so a
		VGM or any other single-song format looks exactly as it did before.
	*/
	juce::ComboBox songBox;
	juce::Label songLabel;			// "Song", from the language pack, left of the box

	void switchToSelectedSong (void);

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SongInfoPanel)
};

} // namespace toybox::ui
