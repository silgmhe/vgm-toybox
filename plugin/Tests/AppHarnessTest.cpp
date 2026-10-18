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
	    press <key>              escape space enter home left right ctrl+o ctrl+1 .. ctrl+9
	    click <componentId>      trigger a button (and report its new state)
	    toggle <componentId>     the same, for the controls that toggle
	    set <componentId> <v>    set a slider or a combo box, with the notification the UI sends
	    dump [<componentId>]     the component tree, or one component, as JSON
	    assert <id> <field> <op> <value>
	                             field: visible enabled checked text
	                             op:    == != contains
	    sleep <ms>               let the message loop run - timers, async callbacks
	    help                     the command list

	Directives (read before the window exists, because they decide what it opens):

	    # settings lastSong <path>          the song the previous session left in settings.xml
	    # settings restoreLastSong on|off   the General page's "reopen the last song" switch
	    # settings languageDir <path>       copy the *.lang files there into the run's i18n folder

	Why this exists rather than screenshots: the failures that actually happened here (a modal window
	that never appeared, a tick that never moved, a song that never loaded) are structure and state,
	and structure survives headless, in CI, at any DPI.
*/

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "Standalone/StandaloneHooks.h"
#include "i18n/LanguageManager.h"
#include "settings/SettingsStore.h"
#include "ui/UiSnapshot.h"

namespace
{

// what the `host` command processes with - the usual small block of a DAW session
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 512;

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
		// The button's own state. For a button that toggles itself that is its value; for the
		// transport and mixer header buttons it is what their panel last *put* there - so run
		// `tick` (refreshUi()) first when asserting on those, or the value is from before the
		// action. That is why `tick` exists as a command.
		if (auto* button = dynamic_cast<juce::Button*> (&component))
		{
			return button->getToggleState() ? "true" : "false";
		}
		return "n/a";
	}
	if (field == "value")
	{
		// The number a slider holds. `text` is the formatted version of it, which for the seek bar
		// is nothing at all (no text box) - the position only exists as this number.
		if (auto* slider = dynamic_cast<juce::Slider*> (&component))
		{
			return juce::String (slider->getValue(), 6);
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
	if (op == "~=")
	{
		// Numbers, with the slack of one audio block: a value the engine publishes after a block
		// has already advanced by that block (512 @ 48 kHz = 10.7 ms), so an exact == would be
		// asserting the block size rather than the mapping. 50 ms is far below anything the sync
		// mapping can be wrong by (a wrong side of the cue is seconds).
		return std::abs (actual.getDoubleValue() - wanted.getDoubleValue()) <= 0.05;
	}

	return false;
}

/** The key names the scripts may use - the ones the application itself handles. */
bool keyFromName (const juce::String& name, juce::KeyPress& out)
{
	const auto lower = name.toLowerCase();

	if (lower == "space") { out = juce::KeyPress (juce::KeyPress::spaceKey); return true; }
	if (lower == "enter") { out = juce::KeyPress (juce::KeyPress::returnKey); return true; }
	if (lower == "escape") { out = juce::KeyPress (juce::KeyPress::escapeKey); return true; }
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

/** Replaces $NAME with the environment variable of that name.

	Scripts live in the repository, so they cannot hard-code the path of a song - the one thing
	they need that is not in the repository. `load $VGM_TEST_SONG` keeps the script portable and
	lets a machine without a song skip it instead of failing (see commandLoad). The directives
	that have to run before the editor exists (`# settings lastSong`) are expanded in main(), so
	this is a free function rather than a Runner method. */
juce::String expandVariables (const juce::String& line)
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
		Pauses between commands, without running the message loop.

		Waiting is what almost every command wants, because what they assert is set **synchronously**
		by the command itself: a load calls songChanged() (which rebuilds the chip card and refreshes
		the panels) and updateStatusLine(), and a key press calls straight into the transport.

		A script that needs the next callback - a TreeView re-laying out its rows, a posted click, a
		timer - asks for it with the sleep command, the only one that runs the loop (see
		commandSleep). That loop is available to this target alone: see the
		JUCE_MODAL_LOOPS_PERMITTED definition in plugin/CMakeLists.txt.
	*/
	static void settle (int milliseconds)
	{
		juce::Thread::sleep (juce::jmax (1, milliseconds));
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
		else if (command == "menu") { commandMenu (tokens); }
		else if (command == "host") { commandHost (tokens); }
		else if (command == "engine") { commandEngine (tokens); }
		else if (command == "tick") { commandTick (); }
		else if (command == "shot") { commandShot (tokens); }
		else if (command == "size") { commandSize (tokens); }
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

		// The load is asynchronous - the engine scans the song for its length before the transport
		// can play it - so wait for the status line to leave "Loading" instead of guessing how long
		// that takes. The scripts assert what it says next, so this is the real barrier.
		const double deadline = juce::Time::getMillisecondCounterHiRes() + 10000.0;
		juce::String status;

		while (juce::Time::getMillisecondCounterHiRes() < deadline)
		{
			// Pump, like the `sleep` command does: the load finishes by posting a message, and
			// settle() is a plain Thread::sleep, which would leave that message unprocessed.
			juce::MessageManager::getInstance()->runDispatchLoopUntil (25);

			if (auto* label = findByID (editor, "editor.status"))
			{
				status = toybox::ui::snapshot::textOf (*label);
			}

			if (! status.startsWith ("Loading"))
			{
				break;
			}
		}

		report (! status.startsWith ("Loading"), "load " + file.getFileName()
					+ "   (" + juce::String (status.isEmpty() ? "no status line" : status) + ")");
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

		// `press <key>` is a key the editor has to handle; `press <key> unhandled` is a key it has to
		// let out - that is how a shortcut reaches the host, and the only way to see it from a
		// script (daw-sync.txt: the space bar belongs to the DAW while it drives the song).
		const bool wantsHandled = tokens.size() < 3 || tokens[2].toLowerCase() != "unhandled";

		const bool handled = editor.keyPressed (key);
		settle (60);
		report (handled == wantsHandled, "press " + tokens[1] + (wantsHandled ? "" : " (unhandled)"));
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

	/** The window's menu model, reached through the real component tree: the menu bar is a
		component with an ID and it knows its model, so no test-only accessor is needed. */
	toybox::ui::MainMenu* findMenu (void) const
	{
		auto* bar = dynamic_cast<juce::MenuBarComponent*> (findByID (editor, "editor.menubar"));
		return (bar != nullptr) ? dynamic_cast<toybox::ui::MainMenu*> (bar->getModel()) : nullptr;
	}

	void commandMenu (const juce::StringArray& tokens)
	{
		auto* menu = findMenu();
		if (menu == nullptr)
		{
			report (false, "menu: the window has no menu model");
			return;
		}

		// `menu invoke file.close` is how a script does what the menu does without it opening
		if (tokens.size() >= 3 && tokens[1].equalsIgnoreCase ("invoke"))
		{
			const bool found = menu->invoke (tokens[2]);
			settle (80);
			report (found, "menu invoke " + tokens[2]);
			return;
		}

		std::printf ("%s", menu->describe().toRawUTF8());
		report (true, "menu");
	}

	void commandTick (void)
	{
		// one frame of the window's own refresh, without a message loop - see the note in
		// ToyboxAudioProcessorEditor::refreshUi()
		editor.refreshUi();
		report (true, "tick");
	}

	/** The DAW's side of the clock: one process block with the position the script asks for.

		The plugin reads the host clock in processBlock() only (a play head cannot be kept), so a
		script that wants to see anything host-driven - a cue, the transport lock - has to make the
		plugin process a block. That is exactly what a wrapper does, with setPlayHead() being JUCE's
		own hook for standing in for one.
	*/
	void commandHost (const juce::StringArray& tokens)
	{
		if (tokens.size() < 2)
		{
			report (false, "host needs a position in seconds");
			return;
		}

		const double seconds = tokens[1].getDoubleValue();
		const bool playing = (tokens.size() < 3) || (tokens[2].toLowerCase() != "stopped");

		if (! prepared)
		{
			processor.setPlayHead (&playHead);
			processor.prepareToPlay (48000.0, kBlockSize);
			prepared = true;
		}

		playHead.set (seconds, playing);

		juce::AudioBuffer<float> buffer (juce::jmax (2, processor.getTotalNumOutputChannels()), kBlockSize);
		juce::MidiBuffer midi;

		// getBusBuffer() returns a view *by value* (it points into `buffer`), and processBlock()
		// takes a non-const reference - binding the temporary directly is an MSVC extension that
		// every other compiler rejects. Hold the view first (the same note as in PluginProcessor).
		auto busBuffer = processor.getBusBuffer (buffer, false, 0);
		processor.processBlock (busBuffer, midi);

		report (true, "host " + juce::String (seconds, 3) + (playing ? " playing" : " stopped"));
	}

	/** A transport command from outside the window, like the tray menu's: it must reach the same
		state the buttons read, not a copy that only the button handlers update. */
	void commandEngine (const juce::StringArray& tokens)
	{
		if (tokens.size() < 2)
		{
			report (false, "engine needs play, pause or stop");
			return;
		}

		const auto action = tokens[1].toLowerCase();
		if (action == "play") { processor.getEngine().play(); }
		else if (action == "pause") { processor.getEngine().pause(); }
		else if (action == "stop") { processor.getEngine().stop(); }
		else
		{
			report (false, "engine: unknown action '" + tokens[1] + "'");
			return;
		}

		report (true, "engine " + action);
	}

	void commandShot (const juce::StringArray& tokens)
	{
		if (tokens.size() < 2)
		{
			report (false, "shot needs a file path");
			return;
		}

		// The window's own pixels, rendered offscreen. This is the deterministic form of a
		// screenshot: no desktop, no window manager, no DPI scaling to argue with - and it includes
		// whatever the commands before it changed, overlays included, because it re-renders the
		// component tree. It exists for the questions text cannot answer ("is this text left
		// aligned?"), which is the one job the pixel path keeps.
		const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, 1.0f);
		const juce::File file (tokens[1]);
		file.deleteFile();

		auto stream = std::unique_ptr<juce::FileOutputStream> (file.createOutputStream());
		if (stream == nullptr)
		{
			report (false, "shot: cannot write " + file.getFullPathName());
			return;
		}

		juce::PNGImageFormat png;
		const bool written = png.writeImageToStream (image, *stream);
		report (written, "shot " + file.getFullPathName() + "  (" + juce::String (image.getWidth())
						   + "x" + juce::String (image.getHeight()) + ")");
	}

	/** The window's own size. The editor is resizable in the product (660x520 minimum), and every
		card is laid out from it - the mixer's rows and its master column are the case that made this
		command worth having: their heights come from the space the card is given. */
	void commandSize (const juce::StringArray& tokens)
	{
		if (tokens.size() < 3)
		{
			report (false, "size needs a width and a height");
			return;
		}

		const int width = tokens[1].getIntValue();
		const int height = tokens[2].getIntValue();

		if (width < 660 || height < 520 || width > 4000 || height > 4000)
		{
			report (false, "size: " + juce::String (width) + "x" + juce::String (height)
							   + " is outside the editor's limits (660x520 or larger)");
			return;
		}

		editor.setSize (width, height);
		juce::MessageManager::getInstance()->runDispatchLoopUntil (40);	// the cards re-lay out
		report (true, "size " + juce::String (width) + "x" + juce::String (height));
	}

	void commandAssert (const juce::StringArray& tokens)
	{
		if (tokens.size() < 5)
		{
			report (false, "assert needs: <id> <field> <op> <value>");
			return;
		}

		// `assert engine position <op> <value>` is the transport's own answer, in seconds. A slider
		// only shows the position normalised (and the window scripts cannot read the engine at
		// all), which is how a seek/start ordering bug in the DAW sync survived: the code ran on
		// every block, but nothing could see where the song actually was.
		if (tokens[1] == "engine" && tokens[2] == "position")
		{
			const auto actual = juce::String (processor.getEngine().getPositionSeconds(), 6);
			report (fieldMatches (actual, tokens[3], tokens[4]),
					"engine.position " + tokens[3] + " '" + tokens[4] + "'   (was: " + actual + ")");
			return;
		}

		// `assert menu.<entry> <field> <op> <value>` reads the menu, which is data rather than a
		// component - that is what makes "the tick follows the page" a text assertion
		if (tokens[1].startsWith ("menu."))
		{
			auto* menu = findMenu();
			toybox::ui::MenuEntry entry;
			if (menu == nullptr || ! menu->findEntry (tokens[1].substring (5), entry))
			{
				report (false, "assert: no menu entry '" + tokens[1] + "'");
				return;
			}

			juce::String actual;
			if (tokens[2] == "checked") { actual = entry.checked ? "true" : "false"; }
			else if (tokens[2] == "enabled") { actual = entry.enabled ? "true" : "false"; }
			else if (tokens[2] == "label") { actual = entry.label; }
			else { report (false, "assert: menu fields are checked, enabled and label"); return; }

			report (fieldMatches (actual, tokens[3], tokens[4]),
					tokens[1] + "." + tokens[2] + " " + tokens[3] + " '" + tokens[4]
						+ "'   (was: " + actual + ")");
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

	/** The one command that runs the message loop, where settle() only waits.

		What that buys a script: everything the application does on its next callback - a TreeView
		recalculating its row widths after being resized (an AsyncUpdater), Button::triggerClick()
		(a posted command message), a change broadcast that is not one of the settings store's.
		Values a command sets synchronously do not need it. */
	void commandSleep (const juce::StringArray& tokens)
	{
		const int milliseconds = juce::jlimit (1, 5000, (tokens.size() >= 2) ? tokens[1].getIntValue() : 100);

		juce::MessageManager::getInstance()->runDispatchLoopUntil (milliseconds);

		report (true, "sleep " + juce::String (milliseconds) + " ms");
	}

	static void printHelp (void)
	{
		std::printf ("    load <path> | press <key> [unhandled] | click <id> | toggle <id> | set <id> <value>\n");
		std::printf ("    unhandled: the key must leave the editor (it is the host's)\n");
		std::printf ("    menu | menu invoke <entryId> | tick | shot <path.png> | size <w> <h>\n");
		std::printf ("    host <seconds> [playing|stopped]   one process block with that host clock\n");
		std::printf ("    engine <play|pause|stop>          a transport command from outside the buttons\n");
		std::printf ("    dump [<id>] | assert <id> <field> <op> <value> | sleep <ms>\n");
		std::printf ("    fields: visible enabled checked text value (components)\n");
		std::printf ("            checked enabled label             (menu entries, as menu.<entryId>)\n");
		std::printf ("    assert engine position <op> <value>   the transport, in seconds\n");
		std::printf ("    ops: == != contains ~= (~= is numeric, within 0.05 = one block of slack)\n");
	}

	/** The clock the `host` command reports - what a wrapper does with setPlayHead(). */
	class FakePlayHead : public juce::AudioPlayHead
	{
	public:
		void set (double secondsToUse, bool playingToUse)
		{
			seconds = secondsToUse;
			playing = playingToUse;
		}

		juce::Optional<PositionInfo> getPosition (void) const override
		{
			PositionInfo info;
			info.setTimeSignature (TimeSignature { 4, 4 });
			info.setBpm (120.0);
			info.setPpqPosition (0.0);
			info.setIsPlaying (playing);
			info.setTimeInSeconds (seconds);
			info.setTimeInSamples ((juce::int64) std::llround (seconds * kSampleRate));
			return info;
		}

	private:
		double seconds = 0.0;
		bool playing = false;
	};

	juce::String skipped;		// set when a load could not run; the rest of the script is skipped
	int skippedCommands = 0;

	ToyboxAudioProcessorEditor& editor;
	ToyboxAudioProcessor& processor;

	FakePlayHead playHead;		// declared after processor: it must outlive it
	bool prepared = false;

	JUCE_DECLARE_NON_COPYABLE (Runner)
};

/** The build a script speaks for, declared on its own first line - `# wrapper vst3`.

	The wrapper type is fixed when the processor is constructed, so it cannot be a command: the
	script has to say it up front. Only the standalone and a hosted build differ in a way the tests
	care about (the File menu, and the DAW-sync controls, which a standalone does not create).
*/
juce::AudioProcessor::WrapperType wrapperTypeFromScript (const juce::File& script)
{
	for (const auto& line : juce::StringArray::fromLines (script.loadFileAsString()))
	{
		const auto trimmed = line.trim();
		if (! trimmed.startsWith ("# wrapper "))
		{
			continue;
		}

		const auto name = trimmed.substring (10).trim().toLowerCase();

		if (name == "vst3")
		{
			return juce::AudioProcessor::wrapperType_VST3;
		}
		if (name == "au")
		{
			return juce::AudioProcessor::wrapperType_AudioUnit;
		}

		return juce::AudioProcessor::wrapperType_Standalone;
	}

	return juce::AudioProcessor::wrapperType_Standalone;
}

/** A settings folder for one run, and its removal.

	The settings page writes as soon as a switch is clicked, and the restore scripts need a
	remembered song; neither may touch the real preferences of the machine the tests run on. The
	folder is created before the processor, because the store reads its location on first use. */
struct TemporarySettingsDirectory
{
	juce::File directory;

	TemporarySettingsDirectory()
		: directory (juce::File::getSpecialLocation (juce::File::tempDirectory)
						 .getNonexistentChildFile ("vgmtoybox-app-test-", {}, false))
	{
		directory.createDirectory();

	   #if JUCE_WINDOWS
		_putenv_s ("VGMTOYBOX_SETTINGS_DIR", directory.getFullPathName().toRawUTF8());
	   #else
		setenv ("VGMTOYBOX_SETTINGS_DIR", directory.getFullPathName().toRawUTF8(), 1);
	   #endif
	}

	~TemporarySettingsDirectory()
	{
		directory.deleteRecursively();
	}
};

/** The startup state a script can ask for before the editor exists.

	`# settings lastSong <path>` writes the remembered song into the run's settings file - what a
	previous session would have left there - and `# settings restoreLastSong on|off` sets the
	switch. These are directives rather than commands because the window decides what to open in
	its constructor and its first timer tick, and the first command runs after both.

	False means the script asked for a song that is not on this machine (VGM_TEST_SONG unset);
	main() then reports the run as skipped, the same way commandLoad() does.
*/
bool applyScriptSettings (const juce::File& script)
{
	auto& store = toybox::settings::SettingsStore::getInstance();

	for (const auto& raw : juce::StringArray::fromLines (script.loadFileAsString()))
	{
		const auto line = expandVariables (raw.trim());
		if (! line.startsWith ("# settings "))
		{
			continue;
		}

		const auto directive = line.fromFirstOccurrenceOf ("# settings ", false, false).trim();
		if (directive.startsWith ("lastSong "))
		{
			const auto path = directive.fromFirstOccurrenceOf ("lastSong ", false, false).trim();
			if (path.startsWithChar ('$'))
			{
				return false;
			}

			store.setLastSong (juce::File (path));
		}
		else if (directive.startsWith ("restoreLastSong "))
		{
			store.setRestoreLastSong (directive.endsWithIgnoreCase (" on"));
		}
		else if (directive.startsWith ("languageDir "))
		{
			// The language manager scans <settings>/i18n when the editor is built. Copying the
			// script's translation files in here keeps the run self-contained: the real user
			// folder is never read or written.
			const auto path = directive.fromFirstOccurrenceOf ("languageDir ", false, false).trim();
			const juce::File source (path);

			if (! source.isDirectory())
			{
				std::printf ("# settings languageDir: not a directory: %s\n", path.toRawUTF8());
				continue;
			}

			const auto target = toybox::i18n::LanguageManager::externalDirectory();
			target.createDirectory();

			for (const auto& file : source.findChildFiles (juce::File::findFiles, false, "*.lang"))
			{
				file.copyFileTo (target.getChildFile (file.getFileName()));
			}
		}
		else
		{
			std::printf ("unknown # settings directive: %s\n", directive.toRawUTF8());
		}
	}

	return true;
}

/** Whether a script speaks for an application that has an audio device of its own - `# hooks audio`.

	The settings panel decides whether it has an Audio page when it is constructed, from
	standalone::available(), which reads the hook table. So a script that wants the three-page shape
	has to say so before the editor exists - the same reason, and the same place, as `# wrapper`.
	The two hooks are fakes: the real device is not what such a script is about, the row and the
	page are.
*/
bool scriptAsksForAudioHooks (const juce::File& script)
{
	for (const auto& line : juce::StringArray::fromLines (script.loadFileAsString()))
	{
		if (line.trim() == "# hooks audio")
		{
			return true;
		}
	}

	return false;
}

} // namespace

int main (int argc, char* argv[])
{
	// GUI objects without a window: this is what makes a console harness possible at all
	juce::ScopedJuceInitialiser_GUI juceInitialiser;

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

	// The harness speaks for the standalone build by default, because that is where the window is
	// ours (Close and Exit exist there, and a song can arrive on the command line). A script that
	// needs a hosted build says so on its first line - `# wrapper vst3` - and that has to be
	// decided before the processor exists, which is why the file is read here. `wrapperType` is
	// const by design, so setTypeOfNextNewPlugin() is JUCE's own hook for exactly this situation.
	const juce::File script (arguments.isEmpty() ? juce::String() : arguments[0]);
	const auto wrapper = script.existsAsFile() ? wrapperTypeFromScript (script)
											   : juce::AudioProcessor::wrapperType_Standalone;
	juce::AudioProcessor::setTypeOfNextNewPlugin (wrapper);

	if (script.existsAsFile() && scriptAsksForAudioHooks (script))
	{
		auto& hooks = toybox::standalone::hooks();
		hooks.describeAudioDevice = [] { return juce::String ("Fake device"); };
		hooks.showAudioSettings = [] {};
	}

	// Every script gets its own settings folder, and the directives that decide what the window
	// restores are applied before it exists: by the first timer tick the decision is already made.
	std::unique_ptr<TemporarySettingsDirectory> settingsForThisRun;
	if (script.existsAsFile())
	{
		settingsForThisRun = std::make_unique<TemporarySettingsDirectory>();

		// The scripts assert English strings, so the machine's system language must not decide
		// what they open with. A script that is about the Language page switches from here like
		// a user does (Tests/scripts/language.txt).
		toybox::settings::SettingsStore::getInstance().setLanguageSelection ("builtin:en", "en");

		if (! applyScriptSettings (script))
		{
			std::printf ("skipped: # settings lastSong (VGM_TEST_SONG is not set)\n");
			return 0;
		}
	}

	ToyboxAudioProcessor processor;
	ToyboxAudioProcessorEditor editor (processor);
	editor.setSize (820, 650);		// lays the tree out; never added to the desktop

	if (arguments.isEmpty())
	{
		const int count = toybox::ui::countComponents (editor);
		std::printf ("=== the window without a window ===\n");
		std::printf ("components: %d\n\n", count);
		std::printf ("%s", toybox::ui::dumpComponentTree (editor).toRawUTF8());
		return count > 0 ? 0 : 1;
	}

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
