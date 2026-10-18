#pragma once

#include <functional>
#include <vector>

#include "CardComponent.h"

namespace toybox { struct ChipEntry; }

namespace toybox::ui
{

/**
	Card with one checkbox per sound chip of the current song (ticked = audible). The list is
	rebuilt when a file is loaded; the card hides itself while the list is empty (no file).
	The section header ("SOUND CHIPS" / "UNCHECK TO MUTE") is painted by the card itself.
*/
class ChipPanel : public CardComponent
{
public:
	using MuteCallback = std::function<void (size_t chipIndex, bool shouldBeMuted)>;

	// the copy/move macros below suppress the implicit default constructor
	ChipPanel (void) = default;

	/** Rebuilds the checkbox list and remembers how to report mute changes. */
	void rebuild (const std::vector<toybox::ChipEntry>& chips, MuteCallback onMuteChanged);

	bool hasChips (void) const { return ! chipButtons.isEmpty(); }

	/** Height the card needs to show every checkbox at this width (0 while empty). */
	int getPreferredHeight (int availableWidth) const;

	/** Re-applies the header and the tooltips; chip and core names themselves are not translated. */
	void refreshLanguage (void);

private:
	void resized (void) override;
	void paintCardContent (juce::Graphics& g) override;

	MuteCallback muteCallback;
	juce::OwnedArray<juce::ToggleButton> chipButtons;
	juce::StringArray chipNames;
	juce::StringArray chipCores;
	juce::Rectangle<int> headerBounds;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChipPanel)
};

} // namespace toybox::ui
