#pragma once

#include <functional>
#include <vector>

#include <juce_core/juce_core.h>

#include "playback/PlaybackEngine.h"

namespace toybox::standalone
{

/**
	The tray icon's context menu, as data rather than as a window.

	The same shape as ui/MainMenu.h, and for the same reason: the menu has to be inspectable
	without a desktop. A test builds it and reads the labels, ticks and enabled flags; the real
	tray backend turns the very same groups into a Win32 menu on Windows, an NSMenu on macOS, and
	JUCE's menu on Linux. Nothing here touches the GUI, so it is compiled into the console test as
	well as into the application.

	The menu is rebuilt every time it is shown, so "Play" becomes "Pause", and the loop and fade
	ticks follow the engine rather than a copy that could drift from it.
*/
/** The glyphs the menu adapters draw beside a row. The enum lives here, outside the GUI, so the
	console test can assert that every row has one without linking a UI toolkit. */
enum class TrayMenuIcon
{
	none,
	showWindow,
	play,
	pause,
	stop,
	back,
	forward,
	loop,
	loop1,
	loop2,
	loop3,
	loopForever,
	fade,
	fadeOff,
	quit
};

struct TrayMenuEntry
{
	juce::String id;			// stable name, e.g. "tray.playPause" - what tests invoke
	juce::String label;
	TrayMenuIcon icon = TrayMenuIcon::none;
	bool ticked = false;
	bool enabled = true;
	bool separatorBefore = false;
	std::function<void()> action;
};

struct TrayMenuGroup
{
	juce::String title;			// empty: an item of the top level; otherwise a submenu
	TrayMenuIcon icon = TrayMenuIcon::none;	// the submenu's own row; unused for a top-level group
	std::vector<TrayMenuEntry> entries;
};

using TrayMenuModel = std::vector<TrayMenuGroup>;

class TrayMenu
{
public:
	TrayMenu (PlaybackEngine& engine,
			  std::function<void()> showWindow,
			  std::function<void()> quit);

	/** The menu as it should appear right now: current play state, current loop and fade. */
	TrayMenuModel build (void) const;

	/** Runs the enabled entry with this id. False for an unknown or disabled entry, which is
		what a menu click on a greyed row would be. */
	bool invoke (const juce::String& id) const;

private:
	PlaybackEngine& engine;
	std::function<void()> showWindow;
	std::function<void()> quit;
};

} // namespace toybox::standalone
