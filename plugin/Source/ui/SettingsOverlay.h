#pragma once

#include "Theme.h"
#include "View.h"
#include "ViewStack.h"
#include "settings/SettingsStore.h"

namespace toybox::ui
{

/**
	The Theme page: which palette the whole application draws with.

	Light and Dark are two views of one stored value, and the store is the value - so this page,
	the Theme menu and the palette actually in force cannot disagree: all three ask
	`settings/SettingsStore.h`. The two options are radio buttons (JUCE keeps a radio group
	exclusive by itself), "Follow system" is the third choice drawn as the checkbox the menu item
	describes, and clicking an option turns the checkbox off - which is exactly what clicking Light
	or Dark in the menu does (docs/desgin/settings-and-theme-design.md sections 6 and 7).

	While the checkbox is on, the line under it says which palette that *is* at the moment: a
	platform that cannot answer then shows "Light" instead of a switch that looks broken
	(section 8).
*/
class ThemeSettingsPage : public View
{
public:
	ThemeSettingsPage (void);

	juce::String getViewName (void) const override { return "Theme"; }

	/** Re-reads the stored mode into the controls. Named like `View::refresh()` because that is
		what it is - only the frequency differs: no window timer drives this stack, so the page is
		refreshed when it comes up and after every click here. */
	void refresh (void) override;

	void paint (juce::Graphics& g) override;
	void resized (void) override;
	void lookAndFeelChanged (void) override;

private:
	/** Stores `mode`, then puts the controls back in step with it. The refresh is here rather than
		in a change listener because the store does nothing when the mode is the one it already has
		(by design, section 4) - a click on the option that was already on changes no state, so it
		broadcasts nothing, and the page has to be right anyway. */
	void select (toybox::settings::SettingsStore::ThemeMode mode);

	/** What "follow system" resolves to at this moment, e.g. "Follow system (now: Dark)". */
	juce::String describeFollowSystem (void) const;

	juce::TextButton lightButton { "Light" };
	juce::TextButton darkButton { "Dark" };
	juce::ToggleButton followButton { "Follow system" };
	juce::Label currentLabel;	// the line under the checkbox, only while it is on

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ThemeSettingsPage)
};

//==============================================================================

/**
	The General page: where the settings live, and how to put them back.

	Both are answers the store already has - the path it read at startup, and the error of the last
	write it attempted - so this page owns no state of its own (section 6).
*/
class GeneralSettingsPage : public View
{
public:
	GeneralSettingsPage (void);

	juce::String getViewName (void) const override { return "General"; }

	/** Re-reads the file path and whatever the store last failed at. */
	void refresh (void) override;

	void paint (juce::Graphics& g) override;
	void resized (void) override;
	void lookAndFeelChanged (void) override;

private:
	// "Reuse the running window": a switch on Windows, a statement on macOS (the system opens a
	// document in the running application, so there is nothing to switch), and neither on Linux
	// (no hand-over exists there - see Source/Standalone/StandaloneApp.cpp). All three are
	// declared; only the one this platform can offer is added to the page.
	juce::ToggleButton reuseWindowButton { "Reuse the running window" };
	juce::Label reuseWindowNote;
	juce::Label reuseWindowHint;

	juce::Label fileCaption;	// "Settings file"
	juce::Label pathLabel;		// the path itself, read-only
	juce::TextButton openFolderButton { "Open folder" };
	juce::TextButton resetButton { "Restore defaults" };
	juce::Label errorLabel;		// one line, and only while the store has something to report

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GeneralSettingsPage)
};

//==============================================================================

/**
	The settings window.

	Drawn inside the window rather than in one of its own, like the About box - ui/AboutOverlay.h
	has the story of what a second window cost this application. Same shape, same reasons: a scrim
	over everything, a centred panel, a click outside it or escape dismisses it, and no editor
	class of its own.

	The sidebar is a TreeView with the root hidden: the two pages are its children, so a future
	settings page is one `addSubItem` (section 6). The pages are the window's own page container
	(ui/ViewStack.h) - the same object the play/mixer/visualiser pages use, so a page here is
	written exactly like a page there.
*/
class SettingsOverlay : public juce::Component
{
public:
	SettingsOverlay (void);
	~SettingsOverlay (void) override = default;

	/** Called when the panel is dismissed - the window hides the overlay. */
	std::function<void()> onClose;

	/** Brings both pages in step with the store and marks the page that is up in the sidebar. The
		window calls this every time the overlay comes up, because the stored values can have been
		changed by the Theme menu while this was hidden. */
	void refreshFromStore (void);

	void paint (juce::Graphics& g) override;
	void resized (void) override;
	void lookAndFeelChanged (void) override;
	void mouseDown (const juce::MouseEvent& event) override;
	bool keyPressed (const juce::KeyPress& key) override;

private:
	// One place decides where each part of the panel is, because two callers need to agree about
	// it: resized() puts a control there, paint() draws the surface it sits on.
	juce::Rectangle<int> panelBounds (void) const;

	/** The panel without the title strip - what the sidebar, the pages and the footer divide up. */
	juce::Rectangle<int> bodyBounds (void) const;

	/** The sidebar, inset by nothing: the tree is put inside it with a small margin. */
	juce::Rectangle<int> sidebarBounds (void) const;

	/** The pages, to the right of the sidebar. */
	juce::Rectangle<int> pageAreaBounds (void) const;

	/** The strip along the bottom that holds the Close button. */
	juce::Rectangle<int> footerBounds (void) const;

	void dismiss (void);

	/** The sidebar's items. The root is a heading, not a page, so it cannot be selected; the two
		sub-items are the pages and own nothing - see ui/AboutOverlay.h for the same reasoning
		about a Component subclass that is deliberately tiny. */
	std::unique_ptr<juce::TreeViewItem> rootItem;

	juce::TreeView tree;	// after the items: the tree has to go first (it does not own them)

	ViewStack pages;
	ThemeSettingsPage* themePage = nullptr;		// owned by `pages`
	GeneralSettingsPage* generalPage = nullptr;	// owned by `pages`

	juce::TextButton closeButton { "Close" };

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsOverlay)
};

} // namespace toybox::ui
