#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace toybox::ui
{

/**
	The application menu: File (open a song, close it, exit), View (which page the window shows)
	and Help (about).

	The window owns one and hands it the actions, so the menu only knows what a menu should know -
	which items exist, what they are called, and which of them make sense here. Close and Exit are
	meaningful only where this plugin owns the application, i.e. in the standalone build: inside a
	host the window belongs to the DAW and quitting would take the host down with it, so those two
	items are left out entirely rather than shown greyed out.

	The View items are not written out here: they come from `viewNames`, which the window fills from
	its ui/ViewStack.h, so adding a page to the stack is what puts it in this menu - and
	`currentView` is what ticks the page that is on screen.

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

	/** Called with the index of the page the user picked (into `viewNames`). */
	std::function<void (int)> onSelectView;

	/** The pages, in menu order - filled from the window's ViewStack. */
	juce::StringArray viewNames;

	/** Asks the window which page is on screen, and is read every time the menu is built.

		Deliberately a question rather than a copy of the index: the first version kept a copy that
		the window was supposed to update, nothing did, and the tick stayed on Play after switching
		pages. There is nothing to keep in step now. */
	std::function<int (void)> getCurrentView;

	/** True in the standalone build - see the class comment. */
	bool standalone = false;

	juce::StringArray getMenuBarNames (void) override
	{
		return { "File", "View", "Help" };
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

		if (index == 1)
		{
			// One item per page, ticked on the one on screen. The accelerators are descriptions
			// like every other one here - the window handles Ctrl+1..9 in keyPressed.
			for (int i = 0; i < viewNames.size(); ++i)
			{
				juce::PopupMenu::Item item { viewNames[i] };
				item.isTicked = (i == currentViewIndex());
				if (i < 9)
				{
					item.shortcutKeyDescription = "Ctrl+" + juce::String (i + 1);
				}
				item.action = [this, i] { if (onSelectView != nullptr) onSelectView (i); };
				menu.addItem (item);
			}

			if (viewNames.isEmpty())
			{
				// a menu can be opened before the window has filled the list in
				menu.addItem (juce::PopupMenu::Item ("No pages yet").setEnabled (false));
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
	/** The page on screen, as the window reports it at this moment. */
	int currentViewIndex (void) const
	{
		return getCurrentView != nullptr ? getCurrentView() : 0;
	}

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
