#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

namespace toybox::ui
{

/** One entry of the application menu, as data - see MainMenu. */
struct MenuEntry
{
	juce::String id;			// "file.open", "view.play" - what a script invokes
	juce::String label;
	juce::String shortcut;		// a description; the window binds the key itself
	bool checked = false;
	bool enabled = true;
	bool separatorBefore = false;
	std::function<void (void)> action;
};

/** One top-level menu and its entries. */
struct MenuGroup
{
	juce::String title;
	std::vector<MenuEntry> entries;
};

/**
	The application menu: File (open a song, close it, exit), View (which page the window shows)
	and Help (about) - held as **data**, which is the point of this class.

	JUCE cannot enumerate a PopupMenu, so nothing could ask what the menu says or which item is
	ticked; the only way to check "does the tick follow the page" was a screenshot. `buildGroups()`
	is the single source now: `getMenuForIndex()` renders it for JUCE, and `describe()` prints it as
	text for the console harness (`assert menu.view.visualiser checked == true` in a script). The
	page items come from `viewNames` (the window's ViewStack) and their tick from `getCurrentView`,
	which is asked every time rather than remembered - that bug is why.

	The window owns one and hands it the actions. Close and Exit are meaningful only where this
	plugin owns the application, i.e. the standalone build: inside a host the window belongs to the
	DAW and quitting would take the host down with it, so those items are left out entirely rather
	than shown greyed out. The shortcuts are descriptions too - the editor handles the keys itself
	(see ToyboxAudioProcessorEditor::keyPressed).
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

	/** Asks the window which page is on screen, and is read every time the menu is built. */
	std::function<int (void)> getCurrentView;

	/** True in the standalone build - see the class comment. */
	bool standalone = false;

	/** The menu as data, in menu order - the one place the items are defined. */
	std::vector<MenuGroup> buildGroups (void) const
	{
		std::vector<MenuGroup> groups;

		MenuGroup file { "File", {} };

		MenuEntry open;
		open.id = "file.open";
		open.label = "Open Song...";
		open.shortcut = "Ctrl+O";
		open.action = onOpenSong;
		file.entries.push_back (std::move (open));

		if (standalone)
		{
			MenuEntry close;
			close.id = "file.close";
			close.label = "Close Song";
			close.action = onCloseSong;
			file.entries.push_back (std::move (close));

			MenuEntry exit;
			exit.id = "file.exit";
			exit.label = "Exit";
			exit.separatorBefore = true;
			exit.action = onQuit;
			file.entries.push_back (std::move (exit));
		}

		groups.push_back (std::move (file));

		// One item per page, ticked on the one on screen. The accelerators are descriptions like
		// every other one here - the window handles Ctrl+1..9 in keyPressed.
		MenuGroup view { "View", {} };
		for (int i = 0; i < viewNames.size(); ++i)
		{
			MenuEntry page;
			page.id = "view." + viewNames[i].toLowerCase();
			page.label = viewNames[i];
			page.checked = (i == currentViewIndex());
			if (i < 9)
			{
				page.shortcut = "Ctrl+" + juce::String (i + 1);
			}

			const int index = i;
			page.action = [this, index] { if (onSelectView != nullptr) onSelectView (index); };
			view.entries.push_back (std::move (page));
		}

		if (viewNames.isEmpty())
		{
			// a menu can be opened before the window has filled the list in
			MenuEntry none;
			none.id = "view.none";
			none.label = "No pages yet";
			none.enabled = false;
			view.entries.push_back (std::move (none));
		}

		groups.push_back (std::move (view));

		MenuGroup help { "Help", {} };
		MenuEntry about;
		about.id = "help.about";
		about.label = "About VGMToybox...";
		about.action = onAbout;
		help.entries.push_back (std::move (about));
		groups.push_back (std::move (help));

		return groups;
	}

	/** The groups as text: the title, then one line per entry with its state.

		This is what the console harness prints (`menu View`), and what its
		`assert menu.<id> checked == true` reads. */
	juce::String describe (void) const
	{
		juce::String text;

		for (const auto& group : buildGroups())
		{
			text << group.title << juce::newLine;

			for (const auto& entry : group.entries)
			{
				text << "  " << entry.id.paddedRight (' ', 24)
					 << entry.label.paddedRight (' ', 22);

				if (entry.shortcut.isNotEmpty())
				{
					text << entry.shortcut.paddedRight (' ', 10);
				}
				else
				{
					text << juce::String().paddedRight (' ', 10);
				}

				if (entry.checked)
				{
					text << "checked ";
				}
				if (! entry.enabled)
				{
					text << "disabled";
				}

				text << juce::newLine;
			}
		}

		return text;
	}

	/** Finds an entry by id; null when there is no such entry. */
	const MenuEntry* findEntry (const juce::String& id) const
	{
		for (const auto& group : buildGroups())
		{
			for (const auto& entry : group.entries)
			{
				if (entry.id == id)
				{
					return &entry;
				}
			}
		}

		return nullptr;
	}

	/** Runs the action of the entry with this id.

		False when there is no such entry **or it has no action** - a caller (the console harness
		presses "Close Song" this way) has to be able to tell "the window does not do this" from
		"done", and answering true for an empty action would hide exactly that. */
	bool invoke (const juce::String& id)
	{
		const MenuEntry* found = findEntry (id);
		if (found == nullptr || found->action == nullptr)
		{
			return false;
		}

		// the entry is a copy inside the returned vector, so its action is safe to call here
		found->action();
		return true;
	}

	juce::StringArray getMenuBarNames (void) override
	{
		return { "File", "View", "Help" };
	}

	juce::PopupMenu getMenuForIndex (int index, const juce::String&) override
	{
		const auto groups = buildGroups();
		juce::PopupMenu menu;

		if (! juce::isPositiveAndBelow (index, groups.size()))
		{
			return menu;
		}

		for (const auto& entry : groups[(size_t) index].entries)
		{
			if (entry.separatorBefore)
			{
				menu.addSeparator();
			}

			auto action = entry.action;
			juce::PopupMenu::Item item { entry.label };
			item.isTicked = entry.checked;
			item.isEnabled = entry.enabled;
			item.shortcutKeyDescription = entry.shortcut;
			item.action = [action] { if (action != nullptr) action(); };
			menu.addItem (item);
		}

		return menu;
	}

	void menuItemSelected (int, int) override {}	// every item carries its own action

private:
	/** The page on screen, as the window reports it at this moment. */
	int currentViewIndex (void) const
	{
		return getCurrentView != nullptr ? getCurrentView() : 0;
	}
};

} // namespace toybox::ui
