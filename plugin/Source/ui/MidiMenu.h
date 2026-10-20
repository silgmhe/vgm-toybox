#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace toybox::ui
{

/** The MIDI channel selector's popup: "Off" first, then one entry per channel, each channel
	carrying the MIDI mark in front of its text.

	`selectedItemId` follows the selector's ids (1 = Off, 2..17 = channels 1..16), so the caller
	can hand its own selection straight in and the menu ticks it.

	Separate from the combo box so `vgm_ui_test` can walk the menu without opening one: a popup is
	a window, and "does every channel entry carry the icon" has to be answerable without a window.
*/
inline juce::PopupMenu makeMidiChannelMenu (const juce::String& offText,
											const juce::StringArray& channelTexts,
											int selectedItemId, const juce::Image& channelIcon)
{
	juce::PopupMenu menu;
	menu.addItem (1, offText, true, selectedItemId <= 1, juce::Image());

	for (int index = 0; index < channelTexts.size(); ++index)
	{
		const int itemId = index + 2;
		menu.addItem (itemId, channelTexts[index], true, itemId == selectedItemId, channelIcon);
	}

	return menu;
}

} // namespace toybox::ui
