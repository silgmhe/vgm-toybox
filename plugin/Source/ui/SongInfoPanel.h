#pragma once

#include "CardComponent.h"

namespace toybox { class PlaybackEngine; }

namespace toybox::ui
{

/**
	The "song info" card: title, the most important tags and the file name with the play
	position. Everything is pulled from the engine in refresh(), so the editor only has to
	call that once per timer tick - there is no state pushed in from the outside.

	**It is also the one card that changes the engine.** Picking a song rebuilds it on the load worker
	(`PlaybackEngine::selectSongAsync`), which is why this panel holds a mutable reference and why the
	rebuild's failure comes back as `onSwitchFailure` instead of being swallowed here.
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

	/** **Told when a switch could not be built.** The panel restores its own selection; saying why is
		the window's business, so it goes out as a value and the panel owns no status line.
	*/
	std::function<void (const juce::String&)> onSwitchFailure;

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
