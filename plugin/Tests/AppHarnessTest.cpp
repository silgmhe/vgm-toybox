/*
	Console harness for the window - the runner of docs/desgin/testing-architecture.md.

	It builds the real ToyboxAudioProcessor and ToyboxAudioProcessorEditor, lays them out at a real
	size (no window, no desktop, no sound device), then reads a **script** of text commands, drives the
	same components the application drives, and prints one line per command: `ok` or `FAIL`. The exit
	code is non-zero as soon as anything failed, which is what CI reads.

	    vgm_app_test                 # no script: report the component tree (the phase 0 probe)
	    vgm_app_test <script.txt>    # run a script
	    vgm_app_test --dump <script.txt>

	Commands (one per line, `#` starts a comment, arguments may be quoted):

	    load <path>              a file dropped on the window - the same path the drag & drop uses
	    press <key>              space enter home left right ctrl+o ctrl+1 .. ctrl+9
	    click <componentId>      trigger a button (and report its new state)
	    toggle <componentId>     the same, for the controls that toggle
	    set <componentId> <v>    set a slider or a combo box, with the notification the UI sends
	    dump [<componentId>]     the component tree, or one component, as JSON
	    assert <id> <field> <op> <value>
	                             field: visible enabled checked text
	                             op:    == != contains
	    sleep <ms>               let the message loop run - timers, async callbacks
	    help                     the command list

	Why this exists rather than screenshots: the failures that actually happened here (a modal window
	that never appeared, a tick that never moved, a song that never loaded) are structure and state,
	and structure survives headless, in CI, at any DPI.
*/

#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ui/UiSnapshot.h"

namespace
{

int failures = 0;
bool dumpAfterScript = false;

void report (bool ok, const juce::String& what)
{
	std::printf ("    %s %s\n", ok ? " OK " : "FAIL", what.toRawUTF8());
	if (! ok)
	{
		++failures;
	}
}

/** Splits a line into tokens, honouring "quoted arguments with spaces" (a song path needs them). */
juce::StringArray tokenise (const juce::String& line)
{
	juce::StringArray tokens;
	juce::String current;
	bool inQuotes = false;

	for (const auto character : line)
	{
		if (character == '"')
		{
			inQuotes = ! inQuotes;
		}
		else if (character == ' ' && ! inQuotes)
		{
			if (current.isNotEmpty())
			{
				tokens.add (current);
				current.clear();
			}
		}
		else
		{
			current << character;
		}
	}

	if (current.isNotEmpty())
	{
		tokens.add (current);
	}

	return tokens;
}

/** One line of the tree for a component, looked up by its ID. */
juce::Component* findByID (juce::Component& root, const juce::String& id)
{
	if (id.isEmpty())
	{
		return nullptr;
	}

	if (root.getComponentID() == id)
	{
		return &root;
	}

	for (auto* child : root.getChildren())
	{
		if (child != nullptr)
		{
			if (auto* found = findByID (*child, id))
			{
				return found;
			}
		}
	}

	return nullptr;
}

/** The value an assertion reads: the same fields the snapshot prints. */
juce::String fieldOf (juce::Component& component, const juce::String& field)
{
	if (field == "visible")
	{
		return component.isVisible() ? "true" : "false";
	}
	if (field == "enabled")
	{
		return component.isEnabled() ? "true" : "false";
	}
	if (field == "checked")
	{
		// Only a button that toggles itself answers this honestly. The transport and mixer header
		// buttons are lit by their panel's refresh() - which the harness does not run (no message
		// loop, see settle()) - so reading getToggleState() there would report a value that has
		// nothing to do with what the user sees. Saying so is better than a plausible number.
		if (auto* button = dynamic_cast<juce::Button*> (&component))
		{
			return button->getClickingTogglesState()
					   ? (button->getToggleState() ? "true" : "false")
					   : "n/a (not a toggle button)";
		}
		return "n/a";
	}
	if (field == "text")
	{
		return toybox::ui::snapshot::textOf (component);
	}

	return "?";
}

bool fieldMatches (const juce::String& actual, const juce::String& op, const juce::String& wanted)
{
	if (op == "==")
	{
		return actual == wanted;
	}
	if (op == "!=")
	{
		return actual != wanted;
	}
	if (op == "contains")
	{
		return actual.containsIgnoreCase (wanted);
	}

	return false;
}

/** The key names the scripts may use - the ones the application itself handles. */
bool keyFromName (const juce::String& name, juce::KeyPress& out)
{
	const auto lower = name.toLowerCase();

	if (lower == "space") { out = juce::KeyPress (juce::KeyPress::spaceKey); return true; }
	if (lower == "enter") { out = juce::KeyPress (juce::KeyPress::returnKey); return true; }
	if (lower == "home") { out = juce::KeyPress (juce::KeyPress::homeKey); return true; }
	if (lower == "left") { out = juce::KeyPress (juce::KeyPress::leftKey); return true; }
	if (lower == "right") { out = juce::KeyPress (juce::KeyPress::rightKey); return true; }

	if (lower.startsWith ("ctrl+") && lower.length() == 6)
	{
		out = juce::KeyPress (lower[5], juce::ModifierKeys::commandModifier, 0);
		return true;
	}

	return false;
}

class Runner
{
public:
	explicit Runner (ToyboxAudioProcessorEditor& editorToUse, ToyboxAudioProcessor& processorToUse)
		: editor (editorToUse), processor (processorToUse)
	{
	}

	void run (const juce::StringArray& lines)
	{
		for (const auto& line : lines)
		{
			const auto trimmed = line.trim();
			if (trimmed.isEmpty() || trimmed.startsWithChar ('#'))
			{
				continue;
			}

			if (skipped != juce::String())
			{
				// Everything after a skipped load depends on that song; running it anyway would
				// report failures that say nothing about the application.
				++skippedCommands;
				continue;
			}

			std::printf ("> %s\n", trimmed.toRawUTF8());
			execute (tokenise (expandVariables (trimmed)));
		}

		if (skipped != juce::String())
		{
			std::printf ("    skipped: %s (and the %d command(s) that depend on it)\n",
						 skipped.toRawUTF8(), skippedCommands);
		}
	}

private:
	/**
		Pauses between commands.

		Deliberately not a message loop: `MessageManager::runDispatchLoopUntil()` only exists when
		JUCE_MODAL_LOOPS_PERMITTED is on, and turning that on for the harness means turning it on for
		the plugin and the VST3 build too - not a trade worth making for a test tool.

		It costs nothing here, because everything the commands assert is set **synchronously** by the
		command itself: a load calls songChanged() (which rebuilds the chip card and refreshes the
		panels) and updateStatusLine(), and a key press calls straight into the transport. What this
		form cannot see is anything that only the 10-15 Hz timer would do - meters moving, a level
		fading - and that is worth knowing when writing a script: those belong to the engine and pixel
		tests, or to the controller extraction that the plan still owes (docs/desgin/testing-architecture.md
		§7 risk 2).
	*/
	static void settle (int milliseconds)
	{
		juce::Thread::sleep (juce::jmax (1, milliseconds));
	}

	/** Replaces $NAME with the environment variable of that name.

		Scripts live in the repository, so they cannot hard-code the path of a song - the one thing
		they need that is not in the repository. `load $VGM_TEST_SONG` keeps the script portable and
		lets a machine without a song skip it instead of failing (see commandLoad). */
	static juce::String expandVariables (const juce::String& line)
	{
		juce::String out;
		for (int i = 0; i < line.length(); ++i)
		{
			if (line[i] != '$')
			{
				out << line[i];
				continue;
			}

			int end = i + 1;
			while (end < line.length() && (juce::CharacterFunctions::isLetterOrDigit (line[end])
										   || line[end] == '_'))
			{
				++end;
			}

			const auto name = line.substring (i + 1, end);
			if (name.isEmpty())
			{
				out << '$';
				continue;
			}

			const auto* value = std::getenv (name.toRawUTF8());
			if (value == nullptr)
			{
				out << "$" << name;		// left as it is - commandLoad() recognises and skips it
			}
			else
			{
				out << value;
			}

			i = end - 1;
		}

		return out;
	}

	void execute (const juce::StringArray& tokens)
	{
		const auto command = tokens[0].toLowerCase();

		if (command == "load") { commandLoad (tokens); }
		else if (command == "press") { commandPress (tokens); }
		else if (command == "click" || command == "toggle") { commandClick (tokens, command == "toggle"); }
		else if (command == "set") { commandSet (tokens); }
		else if (command == "dump") { commandDump (tokens); }
		else if (command == "assert") { commandAssert (tokens); }
		else if (command == "sleep") { commandSleep (tokens); }
		else if (command == "help") { printHelp(); }
		else { report (false, "unknown command '" + tokens[0] + "'"); }
	}

	void commandLoad (const juce::StringArray& tokens)
	{
		if (tokens.size() < 2)
		{
			report (false, "load needs a path");
			return;
		}

		// an unexpanded $NAME means this machine has no song for the script - not a failure
		if (tokens[1].startsWithChar ('$'))
		{
			skipped = tokens[1] + " is not set";
			std::printf ("    skip load: %s\n", skipped.toRawUTF8());
			return;
		}

		const juce::File file (tokens[1]);
		if (! file.existsAsFile())
		{
			report (false, "load: '" + tokens[1] + "' does not exist");
			return;
		}

		// The path the application itself takes for a dropped file. (filesDropped is public on
		// FileDragAndDropTarget; going through the base is what makes that reachable from here.)
		juce::FileDragAndDropTarget& dropTarget = editor;
		dropTarget.filesDropped ({ file.getFullPathName() }, 0, 0);
		settle (200);		// the editor's timer refreshes what the load changed

		report (true, "load " + file.getFileName());
	}

	void commandPress (const juce::StringArray& tokens)
	{
		if (tokens.size() < 2)
		{
			report (false, "press needs a key name");
			return;
		}

		juce::KeyPress key;
		if (! keyFromName (tokens[1], key))
		{
			report (false, "press: unknown key '" + tokens[1] + "'");
			return;
		}

		const bool handled = editor.keyPressed (key);
		settle (60);
		report (handled, "press " + tokens[1]);
	}

	void commandClick (const juce::StringArray& tokens, bool expectToggle)
	{
		if (tokens.size() < 2)
		{
			report (false, "click needs a component id");
			return;
		}

		auto* component = findByID (editor, tokens[1]);
		auto* button = dynamic_cast<juce::Button*> (component);
		if (button == nullptr)
		{
			report (false, "click: no button with the id '" + tokens[1] + "'");
			return;
		}

		// A real click on a button that toggles itself flips the state *before* the click handler
		// runs, so that is what this does - triggerClick() alone only fires the handler (measured:
		// a ToggleButton kept its state). A button that does not toggle itself is lit by its
		// panel's refresh() instead, and this reports that rather than a meaningless boolean.
		if (button->getClickingTogglesState())
		{
			button->setToggleState (! button->getToggleState(), juce::dontSendNotification);
		}

		button->triggerClick();
		settle (60);

		report (true, juce::String (expectToggle ? "toggle " : "click ") + tokens[1]
					   + (button->getClickingTogglesState()
							  ? juce::String (" -> checked=") + (button->getToggleState() ? "true" : "false")
							  : juce::String (" -> sent (its panel decides what it looks like)")));
	}

	void commandSet (const juce::StringArray& tokens)
	{
		if (tokens.size() < 3)
		{
			report (false, "set needs a component id and a value");
			return;
		}

		auto* component = findByID (editor, tokens[1]);
		const double value = tokens[2].getDoubleValue();

		if (auto* slider = dynamic_cast<juce::Slider*> (component))
		{
			// the notification is what the real drag sends, so the engine sees the change
			slider->setValue (value, juce::sendNotificationSync);
			settle (60);
			report (true, "set " + tokens[1] + " = " + juce::String (slider->getValue()));
			return;
		}

		if (auto* box = dynamic_cast<juce::ComboBox*> (component))
		{
			const int index = (int) value;
			if (box->getNumItems() <= 0 || index < 0 || index >= box->getNumItems())
			{
				report (false, "set " + tokens[1] + ": item " + juce::String (index)
							   + " is outside 0.." + juce::String (box->getNumItems() - 1));
				return;
			}

			box->setSelectedItemIndex (index, juce::sendNotificationSync);
			settle (60);
			report (true, "set " + tokens[1] + " = " + box->getText());
			return;
		}

		report (false, "set: '" + tokens[1] + "' is not a slider or a combo box");
	}

	void commandDump (const juce::StringArray& tokens)
	{
		if (tokens.size() >= 2)
		{
			if (auto* component = findByID (editor, tokens[1]))
			{
				std::printf ("%s\n", toybox::ui::snapshot::line (*component).toRawUTF8());
				report (true, "dump " + tokens[1]);
			}
			else
			{
				report (false, "dump: no component with the id '" + tokens[1] + "'");
			}
			return;
		}

		std::printf ("%s", toybox::ui::dumpComponentTree (editor).toRawUTF8());
		report (true, "dump");
	}

	void commandAssert (const juce::StringArray& tokens)
	{
		if (tokens.size() < 5)
		{
			report (false, "assert needs: <id> <field> <op> <value>");
			return;
		}

		auto* component = findByID (editor, tokens[1]);
		if (component == nullptr)
		{
			report (false, "assert: no component with the id '" + tokens[1] + "'");
			return;
		}

		const juce::String actual = fieldOf (*component, tokens[2]);
		const bool ok = fieldMatches (actual, tokens[3], tokens[4]);

		report (ok, tokens[1] + "." + tokens[2] + " " + tokens[3] + " '" + tokens[4]
				   + "'   (was: " + actual + ")");
	}

	void commandSleep (const juce::StringArray& tokens)
	{
		const int milliseconds = (tokens.size() >= 2) ? tokens[1].getIntValue() : 100;
		settle (juce::jlimit (1, 5000, milliseconds));
		report (true, "sleep " + juce::String (milliseconds) + " ms");
	}

	static void printHelp (void)
	{
		std::printf ("    load <path> | press <key> | click <id> | toggle <id> | set <id> <value>\n");
		std::printf ("    dump [<id>] | assert <id> <field> <op> <value> | sleep <ms>\n");
		std::printf ("    fields: visible enabled checked text; ops: == != contains\n");
	}

	juce::String skipped;		// set when a load could not run; the rest of the script is skipped
	int skippedCommands = 0;

	ToyboxAudioProcessorEditor& editor;
	ToyboxAudioProcessor& processor;

	JUCE_DECLARE_NON_COPYABLE (Runner)
};

} // namespace

int main (int argc, char* argv[])
{
	// GUI objects without a window: this is what makes a console harness possible at all
	juce::ScopedJuceInitialiser_GUI juceInitialiser;

	// The harness speaks for the standalone build, because that is where the window is ours (Close
	// and Exit exist there, and a song can arrive on the command line). `wrapperType` is const by
	// design, so this is JUCE's own hook for exactly this situation.
	juce::AudioProcessor::setTypeOfNextNewPlugin (juce::AudioProcessor::wrapperType_Standalone);

	ToyboxAudioProcessor processor;
	ToyboxAudioProcessorEditor editor (processor);
	editor.setSize (820, 650);		// lays the tree out; never added to the desktop

	juce::StringArray arguments;
	for (int i = 1; i < argc; ++i)
	{
		arguments.add (juce::String (argv[i]));
	}

	if (arguments.contains ("--dump"))
	{
		dumpAfterScript = true;
		arguments.removeString ("--dump");
	}

	if (arguments.isEmpty())
	{
		const int count = toybox::ui::countComponents (editor);
		std::printf ("=== the window without a window ===\n");
		std::printf ("components: %d\n\n", count);
		std::printf ("%s", toybox::ui::dumpComponentTree (editor).toRawUTF8());
		return count > 0 ? 0 : 1;
	}

	const juce::File script (arguments[0]);
	if (! script.existsAsFile())
	{
		std::printf ("script not found: %s\n", script.getFullPathName().toRawUTF8());
		return 2;
	}

	std::printf ("=== %s ===\n", script.getFileName().toRawUTF8());

	Runner runner (editor, processor);
	runner.run (juce::StringArray::fromLines (script.loadFileAsString()));

	if (dumpAfterScript)
	{
		std::printf ("%s", toybox::ui::dumpComponentTree (editor).toRawUTF8());
	}

	std::printf ("Result    : %s\n", (failures == 0) ? "all checks passed" : "FAILURES");
	return (failures == 0) ? 0 : 1;
}
