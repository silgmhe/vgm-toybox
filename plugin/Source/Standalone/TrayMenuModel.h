#pragma once

#include <functional>
#include <vector>

#include <juce_core/juce_core.h>

namespace toybox::standalone
{

/** The glyphs the menu adapters draw beside a row. The enum lives here, outside the GUI, so the
	console test can assert the icon/tick policy without linking a UI toolkit.

	Policy: a top-level command row carries a glyph; a submenu row uses the platform's native
	check mark and carries no glyph. `ticked` and a glyph are therefore never both set. */
enum class TrayMenuIcon
{
	none,
	showWindow,
	play,
	pause,
	stop,
	loop,
	fade,
	quit
};

struct TrayMenuEntry
{
	juce::String id;			// stable name, e.g. "tray.play" - what tests invoke
	juce::String label;
	TrayMenuIcon icon = TrayMenuIcon::none;	// top-level rows only; submenu rows use `none`
	bool ticked = false;			// native check mark; never combine with `icon`
	bool enabled = true;
	bool separatorBefore = false;
	std::function<void()> action;
};

struct TrayMenuGroup
{
	juce::String title;			// empty for a top-level group; non-empty for a submenu
	TrayMenuIcon icon = TrayMenuIcon::none;	// the submenu's own row; unused for a top-level group
	std::vector<TrayMenuEntry> entries;

	static TrayMenuGroup topLevel (std::vector<TrayMenuEntry> entries);
	static TrayMenuGroup submenu (juce::String title, TrayMenuIcon icon,
								  std::vector<TrayMenuEntry> entries);

	bool isSubmenu (void) const noexcept { return title.isNotEmpty(); }
};

using TrayMenuModel = std::vector<TrayMenuGroup>;

/** Runs the action for a stable entry id. False when the id is unknown or the row is disabled. */
using TrayMenuInvoker = std::function<bool (const juce::String&)>;

/** Receives one depth-first pass over a menu model. Platform adapters implement this and do
	nothing else with the tree: ordering, automatic submenu separators and per-entry separators
	are decided once, in renderTrayMenu(), rather than in each of the three adapters. */
class TrayMenuRenderer
{
public:
	virtual ~TrayMenuRenderer() = default;

	virtual void addSeparator (void) = 0;
	virtual void addItem (const TrayMenuEntry& entry) = 0;
	virtual void beginSubmenu (const TrayMenuGroup& group) = 0;
	virtual void endSubmenu (void) = 0;
};

void renderTrayMenu (const TrayMenuModel& model, TrayMenuRenderer& renderer);

} // namespace toybox::standalone
