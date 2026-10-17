#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

namespace toybox::ui
{

/**
	The window's commands as data - an id, the key that runs it, and what it does - so the menu
	and the keyboard cannot disagree about an accelerator (review M-3/M-4).

	The two used to be separate: ui/MainMenu.h printed `shortcut` as a string, and
	ToyboxAudioProcessorEditor::keyPressed() matched the keys by hand. A string can be typo'd,
	translated or renamed without the matcher noticing, and it cannot be compared against the key
	the window actually receives. Here the menu asks this table for the KeyPress and describes it
	with describe(), and keyPressed() asks the same table which command claims the key.

	The View pages are not registered here: their number and names belong to the window's
	ViewStack, so the menu and the window both derive page N's key from pageKey() instead of
	registering nine commands that would go stale the moment a page is added or removed.
*/
class EditorCommands
{
public:
	struct Command
	{
		juce::String id;			// "file.open" - the same id the menu entry uses
		juce::KeyPress key;			// an empty KeyPress means "no accelerator"
		std::function<void()> action;
	};

	/** Adds a command, or replaces the one with the same id (so a caller may re-register). */
	void add (juce::String id, juce::KeyPress key, std::function<void()> action)
	{
		Command command { std::move (id), key, std::move (action) };

		for (auto& existing : commands)
		{
			if (existing.id == command.id)
			{
				existing = std::move (command);
				return;
			}
		}

		commands.push_back (std::move (command));
	}

	/** The key that runs `id`, or an empty one. The menu describes exactly this KeyPress - a
		shortcut that works is a shortcut the menu can show, because there is one object. */
	juce::KeyPress getKey (const juce::String& id) const
	{
		for (const auto& command : commands)
		{
			if (command.id == id)
			{
				return command.key;
			}
		}

		return {};
	}

	/** Runs the command with this id; false when there is none (or it has no action). */
	bool perform (const juce::String& id) const
	{
		for (const auto& command : commands)
		{
			if (command.id == id && command.action != nullptr)
			{
				command.action();
				return true;
			}
		}

		return false;
	}

	/** Runs the command that owns this key. False when no command does - the window then hands
		the key to the page, which is how the transport keeps its space bar.

		The comparison is KeyPress::operator== (key code and modifiers), not the description:
		the description is for humans and may localise, the equality is what the key event is.
	*/
	bool handleKey (const juce::KeyPress& key) const
	{
		if (key.getKeyCode() == 0)
		{
			return false;
		}

		for (const auto& command : commands)
		{
			if (command.key.getKeyCode() != 0 && command.key == key)
			{
				return perform (command.id);
			}
		}

		return false;
	}

	/** Ctrl+1..9: the accelerator of the Nth View page. */
	static juce::KeyPress pageKey (int index)
	{
		return { '1' + index, juce::ModifierKeys::commandModifier, 0 };
	}

	/** The page a key would switch to, or -1. The mirror of pageKey(), so the menu's description
		and the window's matcher derive the same key instead of both spelling it out. */
	static int pageForKey (const juce::KeyPress& key)
	{
		const auto code = key.getKeyCode();

		if (key.getModifiers().isCommandDown() && code >= '1' && code <= '9')
		{
			return code - '1';
		}

		return -1;
	}

	/** How a key is printed in a menu (and in MainMenu::describe()): empty for "none". */
	static juce::String describe (const juce::KeyPress& key)
	{
		return key.getKeyCode() != 0 ? key.getTextDescriptionWithIcons() : juce::String();
	}

private:
	std::vector<Command> commands;
};

} // namespace toybox::ui
