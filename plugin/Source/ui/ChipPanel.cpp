#include "ChipPanel.h"

#include "Theme.h"

#include "playback/PlaybackEngine.h"

#include <utility>

namespace toybox::ui
{
namespace
{
	constexpr int kRowHeight = 30;			// checkbox (26) + row gap (4)
	constexpr int kButtonWidth = 118;		// minimum width of one checkbox (incl. gap)
}

void ChipPanel::rebuild (const std::vector<toybox::ChipEntry>& chips, MuteCallback onMuteChanged)
{
	muteCallback = std::move (onMuteChanged);
	chipButtons.clear();

	for (size_t i = 0; i < chips.size(); ++i)
	{
		auto* button = new juce::ToggleButton (chips[i].name);
		button->setToggleState (! chips[i].muted, juce::dontSendNotification);	// ticked = audible
		button->setColour (juce::ToggleButton::textColourId, theme::primaryText());
		button->setTooltip (chips[i].core.isNotEmpty()
								? chips[i].name + " (" + chips[i].core + ") - uncheck to mute"
								: chips[i].name + " - uncheck to mute");
		const size_t index = i;
		button->onClick = [this, index, button]
		{
			if (muteCallback)
			{
				muteCallback (index, ! button->getToggleState());
			}
		};
		chipButtons.add (button);
		addAndMakeVisible (button);
	}

	setVisible (hasChips());

	// Lay the new checkboxes out immediately: the editor only calls setBounds(), and
	// Component::setBounds() skips resized() when the card keeps its size - the freshly
	// created buttons would stay at 0 x 0. The repaint clears the pixels of the
	// checkboxes that were just deleted.
	resized();
	repaint();
}

int ChipPanel::getPreferredHeight (int availableWidth) const
{
	if (chipButtons.isEmpty())
	{
		return 0;
	}

	const int perRow = juce::jmax (1, (availableWidth - 2 * theme::cardPadding) / kButtonWidth);
	const int rows = ((int) chipButtons.size() + perRow - 1) / perRow;
	return contentInsetY + textHeaderHeight + rows * kRowHeight;
}

void ChipPanel::resized (void)
{
	headerBounds = getCardHeaderArea (textHeaderHeight);
	auto inner = getCardInnerArea (textHeaderHeight);

	juce::FlexBox flex;
	flex.flexWrap = juce::FlexBox::Wrap::wrap;
	flex.justifyContent = juce::FlexBox::JustifyContent::flexStart;
	flex.alignContent = juce::FlexBox::AlignContent::flexStart;
	for (auto* button : chipButtons)
	{
		flex.items.add (juce::FlexItem (*button).withMinWidth (112.0f).withHeight (26.0f)
							.withMargin ({ 0.0f, 6.0f, 4.0f, 0.0f }));
	}
	flex.performLayout (inner);
}

void ChipPanel::paintCardContent (juce::Graphics& g)
{
	paintCardHeaderRow (g, headerBounds, "SOUND CHIPS", "UNCHECK TO MUTE");
}

} // namespace toybox::ui
