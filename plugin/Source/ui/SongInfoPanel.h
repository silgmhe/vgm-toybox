#pragma once

#include "CardComponent.h"

namespace vgmplay { class MixerEngine; }

namespace vgmplay::ui
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

	explicit SongInfoPanel (const vgmplay::MixerEngine& engineToUse);

	/** Updates title, tags and the file/position line. */
	void refresh (void);

private:
	void resized (void) override;

	const vgmplay::MixerEngine& engine;

	juce::Label titleLabel;
	juce::Label detailsLabel;
	juce::Label fileLabel;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SongInfoPanel)
};

} // namespace vgmplay::ui
