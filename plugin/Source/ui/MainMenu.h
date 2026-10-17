#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "settings/SettingsStore.h"
#include "ui/EditorCommands.h"

namespace toybox::ui
{

/** One entry of the application menu, as data - see MainMenu. */
struct MenuEntry
{
	juce::String id;			// "file.open", "view.play" - what a script invokes
	juce::String label;
	juce::KeyPress key;			// the accelerator itself, shared with the window (EditorCommands)
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
	The application menu: File (open a song, close it, exit), View (which page the window shows),
	Options (the settings window), Theme (which palette the whole application draws with) and Help
	(about) - held as **data**, which is the point of this class.

	Everything in it is the window's state except the Theme items: those belong to the settings
	store, which is asked for the tick on every build and receives the choice - so the palette
	changes for every open window at once, and this menu and the settings window are two views of
	one value rather than two copies of it (docs/desgin/settings-and-theme-design.md section 7).

	JUCE cannot enumerate a PopupMenu, so nothing could ask what the menu says or which item is
	ticked; the only way to check "does the tick follow the page" was a screenshot. `buildGroups()`
	is the single source now: `getMenuForIndex()` renders it for JUCE, and `describe()` prints it as
	text for the console harness (`assert menu.view.visualiser checked == true` in a script). The
	page items come from `viewNames` (the window's ViewStack) and their tick from `getCurrentView`,
	which is asked every time rather than remembered - that bug is why.

	The window owns one and hands it the actions. Close and Exit are meaningful only where this
	plugin owns the application, i.e. the standalone build: inside a host the window belongs to the
	DAW and quitting would take the host down with it, so those items are left out entirely rather
	than shown greyed out.

	The accelerators are not descriptions: `commands` (ui/EditorCommands.h) owns them, and every
	entry that has one asks that table for the KeyPress and renders describe() of it - the window's
	keyPressed() matches the same object, so an accelerator cannot be shown but dead, or work but
	be shown wrong (review M-3).
*/
class MainMenu : public juce::MenuBarModel
{
public:
	std::function<void()> onOpenSong;
	std::function<void()> onCloseSong;
	std::function<void()> onQuit;
	std::function<void()> onAbout;

	/** Options > Settings. The window shows the overlay; nothing here knows what is in it. */
	std::function<void()> onSettings;

	/** Called with the index of the page the user picked (into `viewNames`). */
	std::function<void (int)> onSelectView;

	/** The pages, in menu order - filled from the window's ViewStack. */
	juce::StringArray viewNames;

	/** Asks the window which page is on screen, and is read every time the menu is built. */
	std::function<int (void)> getCurrentView;

	/** True in the standalone build - see the class comment. */
	bool standalone = false;

	/** The window's commands: the accelerators and the actions behind them (ui/EditorCommands.h).
		Null is legal - a menu built before the window has its commands shows no accelerators. */
	const EditorCommands* commands = nullptr;

	/** The menu as data, in menu order - the one place the items are defined. */
	std::vector<MenuGroup> buildGroups (void) const
	{
		std::vector<MenuGroup> groups;

		MenuGroup file { "File", {} };

		MenuEntry open;
		open.id = "file.open";
		open.label = "Open Song...";
		open.key = keyFor ("file.open");
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

		// One item per page, ticked on the one on screen. Ctrl+1..9 comes from pageKey(), the same
		// call the window matches a key against, so the two cannot drift apart.
		MenuGroup view { "View", {} };
		for (int i = 0; i < viewNames.size(); ++i)
		{
			MenuEntry page;
			page.id = "view." + viewNames[i].toLowerCase();
			page.label = viewNames[i];
			page.checked = (i == currentViewIndex());
			if (i < 9)
			{
				page.key = EditorCommands::pageKey (i);
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

		MenuGroup options { "Options", {} };
		MenuEntry settings;
		settings.id = "options.settings";
		settings.label = "Settings...";
		settings.key = keyFor ("options.settings");
		settings.action = onSettings;
		options.entries.push_back (std::move (settings));
		groups.push_back (std::move (options));

		// The palette: one ticked item out of three, and the tick is asked for on every build -
		// the lesson the View menu already learned above. The items write to the store and nothing
		// else: the store broadcasts, and every window (this one included) puts the palette it
		// reads into force. A menu that also told the window would be a second source of truth.
		using Mode = toybox::settings::SettingsStore::ThemeMode;

		struct ThemeChoice
		{
			Mode mode;
			const char* id;
			const char* label;
		};

		const ThemeChoice choices[] = {
			{ Mode::light,		"theme.light",			"Light" },
			{ Mode::dark,		"theme.dark",			"Dark" },
			{ Mode::followSystem, "theme.followSystem",	"Follow system" },
		};

		const auto stored = toybox::settings::SettingsStore::getInstance().getThemeMode();

		MenuGroup theme { "Theme", {} };
		for (const auto& choice : choices)
		{
			MenuEntry entry;
			entry.id = choice.id;
			entry.label = choice.label;
			entry.checked = (stored == choice.mode);

			const Mode mode = choice.mode;
			entry.action = [mode] { toybox::settings::SettingsStore::getInstance().setThemeMode (mode); };
			theme.entries.push_back (std::move (entry));
		}

		groups.push_back (std::move (theme));

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

				const auto shortcut = EditorCommands::describe (entry.key);

		if (shortcut.isNotEmpty())
				{
					text << shortcut.paddedRight (' ', 10);
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

	/** Fills `out` with **a copy** of the entry with this id; false when there is no such entry.

		A copy rather than a pointer on purpose: the groups are built on demand, so a pointer into
		them dangles the moment this returns. It did - and the caller then called a garbage
		std::function, which answered "no" in one place and corrupted the heap in another. */
	bool findEntry (const juce::String& id, MenuEntry& out) const
	{
		for (const auto& group : buildGroups())
		{
			for (const auto& entry : group.entries)
			{
				if (entry.id == id)
				{
					out = entry;
					return true;
				}
			}
		}

		return false;
	}

	/** Runs the action of the entry with this id.

		False when there is no such entry **or it has no action** - a caller (the console harness
		presses "Close Song" this way) has to be able to tell "the window does not do this" from
		"done", and answering true for an empty action would hide exactly that. */
	bool invoke (const juce::String& id)
	{
		MenuEntry entry;

		if (! findEntry (id, entry) || entry.action == nullptr)
		{
			return false;
		}

		entry.action();		// a copy of the entry, so the action is alive here
		return true;
	}

	/** These names are positional: `getMenuForIndex()` indexes `buildGroups()` with the index it is
		handed, so the order here is the order there, and neither may be sorted on its own. */
	juce::StringArray getMenuBarNames (void) override
	{
		return { "File", "View", "Options", "Theme", "Help" };
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
			item.shortcutKeyDescription = EditorCommands::describe (entry.key);
			item.action = [action] { if (action != nullptr) action(); };
			menu.addItem (item);
		}

		return menu;
	}

	void menuItemSelected (int, int) override {}	// every item carries its own action

private:
	/** The accelerator of the command with this id; empty when there is none (or no table). */
	juce::KeyPress keyFor (const juce::String& id) const
	{
		return commands != nullptr ? commands->getKey (id) : juce::KeyPress();
	}

	/** The page on screen, as the window reports it at this moment. */
	int currentViewIndex (void) const
	{
		return getCurrentView != nullptr ? getCurrentView() : 0;
	}
};

} // namespace toybox::ui
