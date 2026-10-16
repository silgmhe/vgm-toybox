#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace toybox::ui
{

/**
	The application menu: File (open a song, close it, exit) and Help (about).

	The window owns one and hands it the actions, so the menu only knows what a menu should know -
	which items exist, what they are called, and which of them make sense here. Close and Exit are
	meaningful only where this plugin owns the application, i.e. in the standalone build: inside a
	host the window belongs to the DAW and quitting would take the host down with it, so those two
	items are left out entirely rather than shown greyed out.

	Every item carries its own action, so there is no item ID to dispatch on. The shortcuts shown
	next to the items are descriptions only - the editor handles the key itself (see
	ToyboxAudioProcessorEditor::keyPressed).
*/
class MainMenu : public juce::MenuBarModel
{
public:
	std::function<void()> onOpenSong;
	std::function<void()> onCloseSong;
	std::function<void()> onQuit;
	std::function<void()> onAbout;

	/** True in the standalone build - see the class comment. */
	bool standalone = false;

	juce::StringArray getMenuBarNames (void) override
	{
		return { "File", "Help" };
	}

	juce::PopupMenu getMenuForIndex (int index, const juce::String&) override
	{
		juce::PopupMenu menu;

		if (index == 0)
		{
			juce::PopupMenu::Item open { "Open Song..." };
			open.shortcutKeyDescription = "Ctrl+O";
			open.action = [this] { call (onOpenSong); };
			menu.addItem (open);

			if (standalone)
			{
				juce::PopupMenu::Item close { "Close Song" };
				close.action = [this] { call (onCloseSong); };
				menu.addItem (close);

				menu.addSeparator();

				juce::PopupMenu::Item exit { "Exit" };
				exit.action = [this] { call (onQuit); };
				menu.addItem (exit);
			}

			return menu;
		}

		juce::PopupMenu::Item about { "About VGMToybox..." };
		about.action = [this] { call (onAbout); };
		menu.addItem (about);

		return menu;
	}

	void menuItemSelected (int, int) override {}	// the items act for themselves

private:
	/** The window may not have wired everything up (a menu can be opened during construction). */
	static void call (const std::function<void()>& action)
	{
		if (action != nullptr)
		{
			action();
		}
	}
};

} // namespace toybox::ui
