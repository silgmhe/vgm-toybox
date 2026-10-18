#pragma once

#include "Theme.h"
#include "View.h"
#include "ViewStack.h"
#include "i18n/LanguageManager.h"
#include "settings/SettingsStore.h"

#include <vector>

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

	juce::String getViewId (void) const override { return "settings-theme"; }
	juce::String getViewName (void) const override { return i18n::tr ("settings.theme"); }

	/** Re-reads the stored mode into the controls. Named like `View::refresh()` because that is
		what it is - only the frequency differs: no window timer drives this stack, so the page is
		refreshed when it comes up and after every click here. */
	void refresh (void) override;

	void paint (juce::Graphics& g) override;
	void resized (void) override;
	void lookAndFeelChanged (void) override;
	void refreshLanguage (void) override;

private:
	/** Stores `mode`, then puts the controls back in step with it. The refresh is here rather than
		in a change listener because the store does nothing when the mode is the one it already has
		(by design, section 4) - a click on the option that was already on changes no state, so it
		broadcasts nothing, and the page has to be right anyway. */
	void select (toybox::settings::SettingsStore::ThemeMode mode);

	/** What "follow system" resolves to at this moment, e.g. "Follow system (now: Dark)". */
	juce::String describeFollowSystem (void) const;

	juce::TextButton lightButton;
	juce::TextButton darkButton;
	juce::ToggleButton followButton;
	juce::Label currentLabel;	// the line under the checkbox, only while it is on

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ThemeSettingsPage)
};

//==============================================================================

/**
	The General page: how the standalone starts, where the settings live, and how to put them back.

	The path and the error are answers the store already has - the one it read at startup, and the
	error of the last write it attempted - so this page owns no state of its own (section 6).
*/
class GeneralSettingsPage : public View
{
public:
	GeneralSettingsPage (void);

	juce::String getViewId (void) const override { return "settings-general"; }
	juce::String getViewName (void) const override { return i18n::tr ("settings.general"); }

	/** Re-reads the file path and whatever the store last failed at. */
	void refresh (void) override;

	void paint (juce::Graphics& g) override;
	void resized (void) override;
	void lookAndFeelChanged (void) override;
	void refreshLanguage (void) override;

private:
	// "Reuse the running window": a switch on Windows, a statement on macOS (the system opens a
	// document in the running application, so there is nothing to switch), and neither on Linux
	// (no hand-over exists there - see Source/Standalone/StandaloneApp.cpp). All three are
	// declared; only the one this platform can offer is added to the page.
	juce::ToggleButton reuseWindowButton;
	juce::Label reuseWindowNote;
	juce::Label reuseWindowHint;

	// "Reopen the last song at startup": the standalone has a last session, a plugin does not, so
	// the pair is only added when toybox::standalone::available() is true (see the constructor).
	juce::ToggleButton restoreLastSongButton;
	juce::Label restoreLastSongHint;

	// "Keep running in the system tray": also standalone-only - a plugin has no window whose
	// close button it could intercept. The warning is Linux-only, because only Linux desktops
	// may have no notification area at all to put the icon in.
	juce::ToggleButton closeToTrayButton;
	juce::Label closeToTrayHint;
	juce::Label closeToTrayWarning;

	juce::Label fileCaption;	// "Settings file"
	juce::Label pathLabel;		// the path itself, read-only
	juce::TextButton openFolderButton;
	juce::TextButton resetButton;
	juce::Label errorLabel;		// one line, and only while the store has something to report

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GeneralSettingsPage)
};

//==============================================================================

/**
	The Audio page: the device the standalone is playing through, and the dialog that changes it.

	Only the standalone application has an audio device of its own - a plugin is handed the host's -
	so `toybox::standalone::available()` decides whether this page exists at all
	(Source/Standalone/StandaloneHooks.h). In a VST3 or AAX build the overlay never creates it and
	the sidebar has no Audio row.

	The dialog behind the button is JUCE's own - the item the Options menu of JUCE's title bar
	carried before the window became the system's (StandaloneApp.cpp). The other three items on that
	menu save and load the plugin's state, which this application deliberately leaves empty, so
	they are not here: a button that writes an empty file is worse than no button.
*/
class AudioSettingsPage : public View,
						  private juce::Timer
{
public:
	AudioSettingsPage (void);

	juce::String getViewId (void) const override { return "settings-audio"; }
	juce::String getViewName (void) const override { return i18n::tr ("settings.audio"); }

	/** Re-reads the device line. */
	void refresh (void) override;

	void paint (juce::Graphics& g) override;
	void resized (void) override;
	void lookAndFeelChanged (void) override;
	void refreshLanguage (void) override;

private:
	void timerCallback (void) override;

	juce::Label deviceLabel;	// one line: the device, its rate, buffer and output count
	juce::TextButton settingsButton;
	juce::Label hintLabel;

	juce::String shownLine;		// what deviceLabel already says, so most ticks do nothing

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioSettingsPage)
};

//==============================================================================

/**
	The built-in languages and every user file in the settings directory's `i18n` folder.

	The list is rebuilt on open and after Rescan. Each file is its own entry even when its `id:`
	matches a built-in language - that is what lets `简体中文 (built-in)` and two different user
	files called `简体中文 (A)` / `简体中文 (B)` appear side by side. Selecting an entry writes the
	pack id and its language tag; the settings store then tells every open editor to re-read its
	text. Missing keys are reported here but do not block selection: the manager falls back to the
	built-in pack for the same language, then English.
*/
class LanguageSettingsPage : public View
{
public:
	LanguageSettingsPage (void);

	juce::String getViewId (void) const override { return "settings-language"; }
	juce::String getViewName (void) const override { return i18n::tr ("settings.language"); }

	void refresh (void) override;
	void paint (juce::Graphics& g) override;
	void resized (void) override;
	void lookAndFeelChanged (void) override;
	void refreshLanguage (void) override;

private:
	void rebuildList (void);
	void rescan (void);
	void openFolder (void);

	juce::ComboBox languageBox;
	juce::Label sourceLabel;
	juce::Label detailLabel;
	juce::Label warningsLabel;
	juce::TextButton openFolderButton;
	juce::TextButton rescanButton;

	std::vector<i18n::LanguageInfo> shownLanguages;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LanguageSettingsPage)
};

//==============================================================================

/**
	The settings window.

	Drawn inside the window rather than in one of its own, like the About box - ui/AboutOverlay.h
	has the story of what a second window cost this application. Same shape, same reasons: a scrim
	over everything, a centred panel, a click outside it or escape dismisses it, and no editor
	class of its own.

	The sidebar is a TreeView with the root hidden: the pages are its children, so a new settings
	page is a page class, one `addSubItem`, one `pages.addView` and one entry in PageIndex - the
	index the item carries and the order the stack holds have to agree, and the constructor asserts
	that they do (section 6). The pages are the window's own page container
	(ui/ViewStack.h) - the same object the play/mixer/visualiser pages use, so a page here is
	written exactly like a page there.

	Audio is the one page that is not always there: it needs an audio device of its own, which only
	the standalone application has (AudioSettingsPage above).
*/
class SettingsOverlay : public juce::Component
{
public:
	SettingsOverlay (void);
	~SettingsOverlay (void) override = default;

	/** Called when the panel is dismissed - the window hides the overlay. */
	std::function<void()> onClose;

	/** Brings every page in step with the store and marks the page that is up in the sidebar. The
		window calls this every time the overlay comes up, because the stored values can have been
		changed by the Theme menu while this was hidden. */
	void refreshFromStore (void);

	/** Rebuilds the sidebar's labels and asks every page to re-apply its text. Called by the
		editor after the language store broadcasts. */
	void refreshLanguage (void);

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

	/** The sidebar's items. The root is a heading, not a page, so it cannot be selected; the
		sub-items are the pages (two, or three when the Audio page exists) and own nothing - see
		ui/AboutOverlay.h for the same reasoning about a Component subclass that is deliberately
		tiny. */
	std::unique_ptr<juce::TreeViewItem> rootItem;

	juce::TreeView tree;	// after the items: the tree has to go first (it does not own them)

	ViewStack pages;
	ThemeSettingsPage* themePage = nullptr;			// owned by `pages`
	GeneralSettingsPage* generalPage = nullptr;		// owned by `pages`
	LanguageSettingsPage* languagePage = nullptr;	// owned by `pages`
	AudioSettingsPage* audioPage = nullptr;			// owned by `pages`; null in a plugin build

	juce::TextButton closeButton;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsOverlay)
};

} // namespace toybox::ui
