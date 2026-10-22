#include "SettingsOverlay.h"

#include "FontChooserDialog.h"
#include "Theme.h"
#include "Standalone/StandaloneHooks.h"
#include "UiFont.h"
#include "playback/MidiSequence.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace toybox::ui
{

namespace
{
	// The panel, and how it divides: a title strip, a sidebar next to the pages, a footer with the
	// one way out that is always in the same place.
	// The height is a floor rather than a fixed size: the panel shrinks with the window (see
	// panelBounds), and the General page needs the fifth row - the tray switch - to fit at the
	// window's own minimum height.
	constexpr int kPanelWidth = 640;
	constexpr int kPanelHeight = 500;
	constexpr int kPanelPadding = 18;
	constexpr int kTitleHeight = 22;
	constexpr int kTitleGap = 12;
	constexpr int kSidebarWidth = 150;
	constexpr int kSidebarGap = 14;
	constexpr int kFooterGap = 12;
	constexpr int kFooterHeight = 32;		// a button face is 28: the bounds are face + 2*inset
	constexpr int kHeadingHeight = 46;
	// The Language explanation is a full translated sentence and does not fit on one line at
	// the page width. Its heading reserves a second line instead of letting JUCE squash the
	// glyphs horizontally; the page has the room, the General page does not.
	constexpr int kTwoLineHeadingHeight = 64;
	constexpr int kThemeChoiceGroup = 1;	// one radio group per page, so any number will do

	/** The heading both pages start with: what the page is, then what it does. */
	void paintHeading (juce::Graphics& g, const juce::String& title, const juce::String& explanation,
					   juce::Rectangle<int> bounds, int headingHeight = kHeadingHeight,
					   int maximumLines = 1)
	{
		auto area = bounds.reduced (kPanelPadding, 0).removeFromTop (headingHeight);
		auto titleArea = area.removeFromTop (24);

		g.setColour (theme::primaryText());
		g.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
		g.drawText (title, titleArea, juce::Justification::centredLeft, false);

		g.setColour (theme::secondaryText());
		g.setFont (juce::Font (juce::FontOptions (12.5f)));
		// A fitted text with a floor: if a translation ever outgrows even the reserved lines,
		// it is clipped rather than compressed into unreadable glyphs.
		g.drawFittedText (explanation, area, juce::Justification::centredLeft, maximumLines, 0.85f);
	}

	/** Where the controls go: under the heading, both pages starting at the same place. */
	juce::Rectangle<int> pageContent (juce::Rectangle<int> bounds, int headingHeight = kHeadingHeight)
	{
		auto area = bounds.reduced (kPanelPadding, 0);
		area.removeFromTop (headingHeight);
		return area;
	}

	/**
		One row of the sidebar.

		`page` is the page it shows, and -1 for the root: the root is a heading rather than a choice,
		and `TreeViewItem::canBeSelected()` is the switch that says so. It is never drawn at all
		(see `setRootItemVisible` below) - it exists to hold the rows; adding a page is one row
		here, one `addSubItem`, one `pages.addView` and one PageIndex entry.
	*/
	class CategoryItem : public juce::TreeViewItem
	{
	public:
		CategoryItem (const juce::String& itemId, const juce::String& nameToShow, int pageIndex,
					  std::function<void (int)> onPicked)
			: id (itemId), name (nameToShow), page (pageIndex), picked (std::move (onPicked))
		{
		}

		bool canBeSelected (void) const override { return page >= 0; }
		bool mightContainSubItems (void) override { return page < 0; }
		int getItemHeight (void) const override { return 26; }
		juce::String getUniqueName (void) const override { return "settings.category." + id; }
		int getPageIndex (void) const { return page; }

		void setLabel (const juce::String& newName)
		{
			name = newName;
			repaintItem();
		}

		void paintItem (juce::Graphics& g, int width, int height) override
		{
			const bool isPage = page >= 0;
			const bool chosen = isPage && isSelected();

			g.setColour (isPage ? (chosen ? theme::accent() : theme::primaryText())
								: theme::secondaryText());
			g.setFont (juce::Font (juce::FontOptions (isPage ? 13.5f : 12.0f,
														isPage && chosen ? juce::Font::bold
																		 : juce::Font::plain)));
			g.drawText (name, juce::Rectangle<int> (0, 0, width, height).reduced (10, 0),
						juce::Justification::centredLeft, true);
		}

		void itemSelectionChanged (bool nowSelected) override
		{
			if (nowSelected && picked != nullptr)
			{
				picked (page);
			}
		}

	private:
		juce::String id;
		juce::String name;
		int page;
		std::function<void (int)> picked;
	};

	/**
		The sidebar's pages, in the order the stack holds them.

		Written once and used by both the row and `addView`: with the numbers typed twice they can
		drift, and a page inserted between two others would open its neighbour with nothing to show
		for it. The constructor asserts the two lists ended up the same length.
	*/
	enum PageIndex
	{
		appearancePageIndex = 0,
		generalPageIndex = 1,
		languagePageIndex = 2,
		midiPageIndex = 3,
		audioPageIndex = 4
	};
}

//==============================================================================
// Appearance

AppearanceSettingsPage::AppearanceSettingsPage (void)
{
	themeSectionLabel.setComponentID ("settings.appearance.theme");
	fontSectionLabel.setComponentID ("settings.appearance.font");
	lightButton.setComponentID ("settings.theme.light");
	darkButton.setComponentID ("settings.theme.dark");
	followButton.setComponentID ("settings.theme.followSystem");
	currentLabel.setComponentID ("settings.theme.current");
	fontValue.setComponentID ("settings.appearance.font.value");
	chooseFontButton.setComponentID ("settings.appearance.font.choose");
	fontHint.setComponentID ("settings.appearance.font.hint");
	restartLabel.setComponentID ("settings.appearance.font.restart");

	// Light and Dark are radio buttons: JUCE keeps a group exclusive by itself, and clicking the
	// one that is already on does nothing - which is what the store does with that click too, so
	// the two agree about it without either of them having to tell the other.
	lightButton.setClickingTogglesState (true);
	darkButton.setClickingTogglesState (true);
	lightButton.setRadioGroupId (kThemeChoiceGroup);
	darkButton.setRadioGroupId (kThemeChoiceGroup);
	lightButton.onClick = [this] { select (toybox::settings::SettingsStore::ThemeMode::light); };
	darkButton.onClick = [this] { select (toybox::settings::SettingsStore::ThemeMode::dark); };

	// The checkbox is the third choice again - the same stored value the Theme menu's last item
	// carries - so turning it off has to leave a mode behind: the palette that is in force at that
	// moment, which is the one the user is looking at.
	followButton.onClick = [this]
	{
		using Mode = toybox::settings::SettingsStore::ThemeMode;

		select (followButton.getToggleState() ? Mode::followSystem
											  : (theme::systemTheme() == theme::Id::dark ? Mode::dark
																						 : Mode::light));
	};

	themeSectionLabel.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
	fontSectionLabel.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
	themeSectionLabel.setJustificationType (juce::Justification::centredLeft);
	fontSectionLabel.setJustificationType (juce::Justification::centredLeft);

	currentLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
	currentLabel.setJustificationType (juce::Justification::centredLeft);

	fontValue.setFont (juce::Font (juce::FontOptions (13.0f)));
	fontValue.setJustificationType (juce::Justification::centredLeft);

	fontHint.setFont (juce::Font (juce::FontOptions (12.5f)));
	fontHint.setJustificationType (juce::Justification::topLeft);
	fontHint.setMinimumHorizontalScale (0.8f);

	restartLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
	restartLabel.setJustificationType (juce::Justification::centredLeft);

	chooseFontButton.onClick = [this] { chooseFont(); };

	addAndMakeVisible (themeSectionLabel);
	addAndMakeVisible (fontSectionLabel);
	addAndMakeVisible (lightButton);
	addAndMakeVisible (darkButton);
	addAndMakeVisible (followButton);
	addAndMakeVisible (currentLabel);
	addAndMakeVisible (fontValue);
	addAndMakeVisible (chooseFontButton);
	addAndMakeVisible (fontHint);
	addAndMakeVisible (restartLabel);

	refreshLanguage();
	lookAndFeelChanged();
}

void AppearanceSettingsPage::refreshLanguage (void)
{
	themeSectionLabel.setText (i18n::tr ("settings.theme"), juce::dontSendNotification);
	fontSectionLabel.setText (i18n::tr ("settings.appearance.font"), juce::dontSendNotification);
	lightButton.setButtonText (i18n::tr ("settings.theme.light"));
	darkButton.setButtonText (i18n::tr ("settings.theme.dark"));
	followButton.setButtonText (i18n::tr ("settings.theme.followSystem"));
	chooseFontButton.setButtonText (i18n::tr ("settings.appearance.font.choose"));
	fontHint.setText (i18n::tr ("settings.appearance.font.hint"), juce::dontSendNotification);
	restartLabel.setText (i18n::tr ("settings.appearance.font.restart"), juce::dontSendNotification);
	refresh();
	repaint();
}

void AppearanceSettingsPage::select (toybox::settings::SettingsStore::ThemeMode mode)
{
	toybox::settings::SettingsStore::getInstance().setThemeMode (mode);

	// The refresh is here and not in a change listener because a click on the option that is
	// already on changes nothing in the store (SettingsStore.cpp:106-109) - it broadcasts nothing,
	// and the page still has to be left showing what is stored rather than what was clicked.
	refresh();
}

void AppearanceSettingsPage::chooseFont (void)
{
	auto& store = toybox::settings::SettingsStore::getInstance();

	auto dialog = std::make_unique<FontChooserDialog> (store.getFontMode(), store.getFontFamily());

	juce::Component::SafePointer<AppearanceSettingsPage> safeThis (this);

	dialog->onAccept = [safeThis] (toybox::settings::SettingsStore::FontMode mode,
								   const juce::String& family)
	{
		if (safeThis == nullptr)
		{
			return;
		}

		toybox::settings::SettingsStore::getInstance().setFontSelection (mode, family);
		safeThis->refresh();
	};

	juce::DialogWindow::LaunchOptions options;
	options.content.setOwned (dialog.release());
	options.dialogTitle = i18n::tr ("settings.appearance.font.dialogTitle");
	options.dialogBackgroundColour = theme::card();
	options.componentToCentreAround = this;
	options.escapeKeyTriggersCloseButton = true;
	options.useNativeTitleBar = true;
	options.resizable = false;
	options.launchAsync();
}

void AppearanceSettingsPage::refresh (void)
{
	auto& store = toybox::settings::SettingsStore::getInstance();
	const auto mode = store.getThemeMode();

	lightButton.setToggleState (mode == toybox::settings::SettingsStore::ThemeMode::light,
								juce::dontSendNotification);
	darkButton.setToggleState (mode == toybox::settings::SettingsStore::ThemeMode::dark,
							   juce::dontSendNotification);
	followButton.setToggleState (mode == toybox::settings::SettingsStore::ThemeMode::followSystem,
								 juce::dontSendNotification);

	// Only while following: the line says what that choice means right now, and on a platform that
	// cannot answer it says Light - which is the documented fallback, not a broken switch
	// (docs/desgin/settings-and-theme-design.md section 8). The row is reserved either way, so the
	// page does not jump as the checkbox is clicked.
	const bool following = mode == toybox::settings::SettingsStore::ThemeMode::followSystem;
	currentLabel.setText (describeFollowSystem(), juce::dontSendNotification);
	currentLabel.setVisible (following);

	const auto fontMode = store.getFontMode();
	const auto family = store.getFontFamily();
	const auto resolved = fonts::resolvedFamily (fontMode, family);
	const bool customMissing = fontMode == toybox::settings::SettingsStore::FontMode::custom
		&& family.isNotEmpty()
		&& ! fonts::isInstalled (family);

	if (customMissing)
	{
		fontValue.setText (i18n::tr ("settings.appearance.font.missing", family),
						   juce::dontSendNotification);
	}
	else if (fontMode == toybox::settings::SettingsStore::FontMode::custom)
	{
		fontValue.setText (resolved, juce::dontSendNotification);
	}
	else
	{
		fontValue.setText (i18n::tr ("settings.appearance.font.systemValue"),
						   juce::dontSendNotification);
	}
}

juce::String AppearanceSettingsPage::describeFollowSystem (void) const
{
	const auto effective = theme::systemTheme() == theme::Id::dark
		? i18n::tr ("settings.theme.dark")
		: i18n::tr ("settings.theme.light");
	return i18n::tr ("settings.theme.following", effective);
}

void AppearanceSettingsPage::paint (juce::Graphics& g)
{
	paintHeading (g, i18n::tr ("settings.appearance"),
				  i18n::tr ("settings.appearance.explanation"), getLocalBounds());
}

void AppearanceSettingsPage::resized (void)
{
	auto area = pageContent (getLocalBounds());

	themeSectionLabel.setBounds (area.removeFromTop (18));
	area.removeFromTop (4);

	auto choices = area.removeFromTop (kFooterHeight);
	lightButton.setBounds (choices.removeFromLeft (96));
	choices.removeFromLeft (theme::rowGap);
	darkButton.setBounds (choices.removeFromLeft (96));

	area.removeFromTop (theme::cardGap + 2);
	followButton.setBounds (area.removeFromTop (24).removeFromLeft (220));

	area.removeFromTop (4);
	currentLabel.setBounds (area.removeFromTop (18));

	area.removeFromTop (theme::cardGap + 6);

	fontSectionLabel.setBounds (area.removeFromTop (18));
	area.removeFromTop (4);

	auto fontRow = area.removeFromTop (kFooterHeight);
	chooseFontButton.setBounds (fontRow.removeFromRight (104));
	fontRow.removeFromRight (8);
	fontValue.setBounds (fontRow);

	area.removeFromTop (6);
	fontHint.setBounds (area.removeFromTop (34));
	area.removeFromTop (4);
	restartLabel.setBounds (area.removeFromTop (18));
}

void AppearanceSettingsPage::lookAndFeelChanged (void)
{
	// The one colour this page sets on a control it owns: the options and the checkbox take the
	// LookAndFeel's own (card face, accent while on, primary text), which is the point of them
	// being LNF-coloured at all - see PluginEditor::lookAndFeelChanged.
	themeSectionLabel.setColour (juce::Label::textColourId, theme::secondaryText());
	fontSectionLabel.setColour (juce::Label::textColourId, theme::secondaryText());
	currentLabel.setColour (juce::Label::textColourId, theme::secondaryText());
	fontValue.setColour (juce::Label::textColourId, theme::primaryText());
	fontHint.setColour (juce::Label::textColourId, theme::secondaryText());
	restartLabel.setColour (juce::Label::textColourId, theme::secondaryText());
}

//==============================================================================
// General

GeneralSettingsPage::GeneralSettingsPage (void)
{
	// The standalone's "double-click a song" behaviour (Source/Standalone/StandaloneApp.cpp). On
	// Windows it is a real choice, so it is a switch; on macOS the Finder hands the document to the
	// running application whatever this says, so the row states the fact instead of pretending to
	// offer a second option; on Linux there is no hand-over to configure at all.
	//
	// Both shapes carry the same ID on purpose: the row is a choice on Windows and a statement of
	// fact on macOS, and a script asking "is the reuse row here?" should not have to know which.
   #if JUCE_WINDOWS
	reuseWindowButton.setComponentID ("settings.general.reuseWindow");
	reuseWindowButton.onClick = [this]
	{
		toybox::settings::SettingsStore::getInstance().setReuseWindow (reuseWindowButton.getToggleState());
		refresh();
	};
	addAndMakeVisible (reuseWindowButton);
   #elif JUCE_MAC
	reuseWindowNote.setComponentID ("settings.general.reuseWindow");
	reuseWindowNote.setFont (juce::Font (juce::FontOptions (13.0f)));
	addAndMakeVisible (reuseWindowNote);
   #endif

   #if JUCE_WINDOWS || JUCE_MAC
	reuseWindowHint.setComponentID ("settings.general.reuseWindowHint");
	reuseWindowHint.setFont (juce::Font (juce::FontOptions (12.0f)));
	reuseWindowHint.setJustificationType (juce::Justification::topLeft);
	reuseWindowHint.setMinimumHorizontalScale (0.75f);
	addAndMakeVisible (reuseWindowHint);
   #endif

	// The standalone reopens the song it had loaded when it last quit (restoreLastSongIfNeeded()
	// in Source/PluginEditor.cpp; the value lives in SettingsStore). A plugin takes its song from
	// the host's project, so it never gets this row: there is no "last time the app was open".
	if (toybox::standalone::available())
	{
		restoreLastSongButton.setComponentID ("settings.general.restoreLastSong");
		restoreLastSongButton.onClick = [this]
		{
			toybox::settings::SettingsStore::getInstance().setRestoreLastSong (
				restoreLastSongButton.getToggleState());
			refresh();
		};
		addAndMakeVisible (restoreLastSongButton);

		restoreLastSongHint.setComponentID ("settings.general.restoreLastSongHint");
		restoreLastSongHint.setFont (juce::Font (juce::FontOptions (12.0f)));
		restoreLastSongHint.setJustificationType (juce::Justification::topLeft);
		restoreLastSongHint.setMinimumHorizontalScale (0.75f);
		addAndMakeVisible (restoreLastSongHint);

		closeToTrayButton.setComponentID ("settings.general.closeToTray");
		closeToTrayButton.onClick = [this]
		{
			toybox::settings::SettingsStore::getInstance().setCloseToTray (
				closeToTrayButton.getToggleState());
			refresh();
		};
		addAndMakeVisible (closeToTrayButton);

		closeToTrayHint.setComponentID ("settings.general.closeToTrayHint");
		closeToTrayHint.setFont (juce::Font (juce::FontOptions (12.0f)));
		closeToTrayHint.setJustificationType (juce::Justification::topLeft);
		closeToTrayHint.setMinimumHorizontalScale (0.75f);
		addAndMakeVisible (closeToTrayHint);

	   #if JUCE_LINUX
		// The one platform where the icon is not part of the desktop: a session without a
		// notification area accepts the call and shows nothing. Red and bold because that is a
		// way to lose the window, not a preference.
		closeToTrayWarning.setComponentID ("settings.general.closeToTrayWarning");
		closeToTrayWarning.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
		closeToTrayWarning.setJustificationType (juce::Justification::topLeft);
		closeToTrayWarning.setMinimumHorizontalScale (0.75f);
		addAndMakeVisible (closeToTrayWarning);
	   #endif
	}

	fileCaption.setComponentID ("settings.general.pathCaption");
	fileCaption.setFont (juce::Font (juce::FontOptions (12.5f)));

	pathLabel.setComponentID ("settings.general.path");
	pathLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
	pathLabel.setJustificationType (juce::Justification::centredLeft);
	// A path is longer than any panel this window will ever have, and it is the one label here
	// whose whole content matters, so it shrinks instead of being cut off with an ellipsis.
	pathLabel.setMinimumHorizontalScale (0.6f);

	openFolderButton.setComponentID ("settings.general.openFolder");
	openFolderButton.onClick = []
	{
		// The folder, not the file: it is what someone looking for "where is this stored" wants,
		// and it works before the first write, when there is no file to reveal yet.
		auto folder = toybox::settings::SettingsStore::settingsDirectory();
		folder.createDirectory();
		folder.revealToUser();
	};

	resetButton.setComponentID ("settings.general.reset");
	resetButton.onClick = [this]
	{
		// One call, and the store tells every window about it - the palette of the window this
		// panel sits in is one of the things that changes back under it.
		toybox::settings::SettingsStore::getInstance().resetToDefaults();
		refresh();
	};

	errorLabel.setComponentID ("settings.general.error");
	errorLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
	errorLabel.setJustificationType (juce::Justification::centredLeft);

	addAndMakeVisible (fileCaption);
	addAndMakeVisible (pathLabel);
	addAndMakeVisible (openFolderButton);
	addAndMakeVisible (resetButton);
	addAndMakeVisible (errorLabel);

	refreshLanguage();
	lookAndFeelChanged();
}

void GeneralSettingsPage::refreshLanguage (void)
{
   #if JUCE_WINDOWS
	reuseWindowButton.setButtonText (i18n::tr ("settings.general.reuseWindow"));
	reuseWindowButton.setTooltip (i18n::tr ("settings.general.reuseWindowTooltip"));
	reuseWindowHint.setText (i18n::tr ("settings.general.reuseWindowHint"), juce::dontSendNotification);
   #elif JUCE_MAC
	reuseWindowNote.setText (i18n::tr ("settings.general.reuseWindowAlways"),
							 juce::dontSendNotification);
	reuseWindowHint.setText (i18n::tr ("settings.general.reuseWindowMacHint"),
							 juce::dontSendNotification);
   #endif

	if (toybox::standalone::available())
	{
		restoreLastSongButton.setButtonText (i18n::tr ("settings.general.restoreLastSong"));
		restoreLastSongButton.setTooltip (i18n::tr ("settings.general.restoreLastSongTooltip"));
		restoreLastSongHint.setText (i18n::tr ("settings.general.restoreLastSongHint"),
									 juce::dontSendNotification);

		closeToTrayButton.setButtonText (i18n::tr ("settings.general.closeToTray"));
		closeToTrayButton.setTooltip (i18n::tr ("settings.general.closeToTrayTooltip"));
		closeToTrayHint.setText (i18n::tr ("settings.general.closeToTrayHint"),
								 juce::dontSendNotification);

	   #if JUCE_LINUX
		closeToTrayWarning.setText (i18n::tr ("settings.general.closeToTrayWarning"),
									juce::dontSendNotification);
	   #endif
	}

	fileCaption.setText (i18n::tr ("settings.general.settingsFile"), juce::dontSendNotification);
	openFolderButton.setButtonText (i18n::tr ("settings.general.openFolder"));
	resetButton.setButtonText (i18n::tr ("settings.general.restoreDefaults"));

	refresh();
	repaint();
}

void GeneralSettingsPage::refresh (void)
{
	auto& store = toybox::settings::SettingsStore::getInstance();

   #if JUCE_WINDOWS
	// Stored, not read back from anything else: a second launch reads the file before any window
	// exists, so the file is the only authority (SettingsStore.h says why).
	reuseWindowButton.setToggleState (store.getReuseWindow(), juce::dontSendNotification);
   #endif

	if (toybox::standalone::available())
	{
		restoreLastSongButton.setToggleState (store.getRestoreLastSong(),
											  juce::dontSendNotification);
		closeToTrayButton.setToggleState (store.getCloseToTray(), juce::dontSendNotification);
	}

	pathLabel.setText (toybox::settings::SettingsStore::settingsFile().getFullPathName(),
					   juce::dontSendNotification);

	// The store writes every change as it happens, so this is the answer to the last write it
	// attempted: a host that forbids the file, a read-only profile. It is said on one quiet line
	// and the window carries on - losing a preference is not a reason to fail (section 4).
	const auto error = store.lastError();
	errorLabel.setText (error, juce::dontSendNotification);
	errorLabel.setVisible (error.isNotEmpty());
}

void GeneralSettingsPage::paint (juce::Graphics& g)
{
	paintHeading (g, i18n::tr ("settings.general"),
				  i18n::tr ("settings.general.explanation"), getLocalBounds());
}

void GeneralSettingsPage::resized (void)
{
	auto area = pageContent (getLocalBounds());

   #if JUCE_WINDOWS
	reuseWindowButton.setBounds (area.removeFromTop (24));
	reuseWindowHint.setBounds (area.removeFromTop (34));
	area.removeFromTop (theme::cardGap);
   #elif JUCE_MAC
	reuseWindowNote.setBounds (area.removeFromTop (20));
	reuseWindowHint.setBounds (area.removeFromTop (34));
	area.removeFromTop (theme::cardGap);
   #endif

	if (toybox::standalone::available())
	{
		restoreLastSongButton.setBounds (area.removeFromTop (24));
		restoreLastSongHint.setBounds (area.removeFromTop (34));
		area.removeFromTop (theme::cardGap);

		closeToTrayButton.setBounds (area.removeFromTop (24));
		closeToTrayHint.setBounds (area.removeFromTop (34));
		area.removeFromTop (theme::cardGap);

	   #if JUCE_LINUX
		closeToTrayWarning.setBounds (area.removeFromTop (34));
		area.removeFromTop (theme::cardGap);
	   #endif
	}

	fileCaption.setBounds (area.removeFromTop (16));
	pathLabel.setBounds (area.removeFromTop (20));

	// Tighter than the gaps above: the switch rows need the height, and the path and its
	// buttons still read as one block at this distance.
	area.removeFromTop (theme::cardGap);
	auto buttons = area.removeFromTop (kFooterHeight);
	openFolderButton.setBounds (buttons.removeFromLeft (112));
	buttons.removeFromLeft (theme::rowGap);
	resetButton.setBounds (buttons.removeFromLeft (146));

	area.removeFromTop (theme::cardGap);
	errorLabel.setBounds (area.removeFromTop (18));
}

void GeneralSettingsPage::lookAndFeelChanged (void)
{
	// The caption is an explanation and the error line is a warning; the path itself is read, so
	// it takes the LookAndFeel's primary text colour and needs nothing said about it here. The
	// reuse note is a read value like the path; its hint is the explanation.
	fileCaption.setColour (juce::Label::textColourId, theme::secondaryText());
	reuseWindowHint.setColour (juce::Label::textColourId, theme::secondaryText());
	reuseWindowNote.setColour (juce::Label::textColourId, theme::primaryText());
	restoreLastSongHint.setColour (juce::Label::textColourId, theme::secondaryText());
	closeToTrayHint.setColour (juce::Label::textColourId, theme::secondaryText());
	closeToTrayWarning.setColour (juce::Label::textColourId, theme::error());
	errorLabel.setColour (juce::Label::textColourId, theme::error());
}

//==============================================================================
// Audio

AudioSettingsPage::AudioSettingsPage (void)
{
	deviceLabel.setComponentID ("settings.audio.device");
	deviceLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
	deviceLabel.setJustificationType (juce::Justification::centredLeft);
	// A driver names a device as long as it likes ("Speakers (Realtek(R) Audio)"), and this is the
	// one line on the page worth reading, so it shrinks rather than being cut off.
	deviceLabel.setMinimumHorizontalScale (0.7f);

	settingsButton.setComponentID ("settings.audio.open");
	settingsButton.onClick = []
	{
		if (auto& table = toybox::standalone::hooks(); table.showAudioSettings != nullptr)
		{
			table.showAudioSettings();
		}
	};

	hintLabel.setComponentID ("settings.audio.hint");
	hintLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
	hintLabel.setJustificationType (juce::Justification::topLeft);
	hintLabel.setMinimumHorizontalScale (0.75f);

	addAndMakeVisible (deviceLabel);
	addAndMakeVisible (settingsButton);
	addAndMakeVisible (hintLabel);

	refreshLanguage();
	lookAndFeelChanged();

	// The dialog is modal and says nothing when it closes, so the line is re-read while this page
	// exists: that is how the new device shows up, and headphones plugged in are noticed too. Two
	// string comparisons a second, and only while the panel is on screen.
	startTimer (500);
}

void AudioSettingsPage::refresh (void)
{
	juce::String line;

	if (auto& table = toybox::standalone::hooks(); table.describeAudioDevice != nullptr)
	{
		line = table.describeAudioDevice();
	}

	if (line.isEmpty())
	{
		line = i18n::tr ("settings.audio.noDevice");
	}

	if (line == shownLine)
	{
		return;
	}

	shownLine = line;
	deviceLabel.setText (line, juce::dontSendNotification);
}

void AudioSettingsPage::timerCallback (void)
{
	if (isShowing())
	{
		refresh();
	}
}

void AudioSettingsPage::refreshLanguage (void)
{
	settingsButton.setButtonText (i18n::tr ("settings.audio.open"));
	hintLabel.setText (i18n::tr ("settings.audio.hint"), juce::dontSendNotification);
	shownLine.clear();		// force refresh() to re-apply the placeholder if it is showing
	refresh();
	repaint();
}

void AudioSettingsPage::paint (juce::Graphics& g)
{
	paintHeading (g, i18n::tr ("settings.audio"),
				  i18n::tr ("settings.audio.explanation"), getLocalBounds());
}

void AudioSettingsPage::resized (void)
{
	auto area = pageContent (getLocalBounds());

	deviceLabel.setBounds (area.removeFromTop (22));
	area.removeFromTop (theme::cardGap + 6);
	settingsButton.setBounds (area.removeFromTop (kFooterHeight).removeFromLeft (196));
	area.removeFromTop (theme::cardGap + 6);
	hintLabel.setBounds (area.removeFromTop (44));
}

void AudioSettingsPage::lookAndFeelChanged (void)
{
	// The device line is a read value like the settings path on the General page, so it takes the
	// LookAndFeel's primary text colour; the hint is the explanation.
	hintLabel.setColour (juce::Label::textColourId, theme::secondaryText());
}

//==============================================================================
// Language

LanguageSettingsPage::LanguageSettingsPage (void)
{
	languageBox.setComponentID ("settings.language.select");
	languageBox.setTooltip (i18n::tr ("settings.language.selectTooltip"));
	languageBox.onChange = [this]
	{
		const int index = languageBox.getSelectedId() - 1;

		if (index < 0 || index >= (int) shownLanguages.size())
		{
			return;
		}

		if (! i18n::LanguageManager::getInstance().selectById (shownLanguages[(std::size_t) index].id))
		{
			refresh();
		}
	};
	addAndMakeVisible (languageBox);

	sourceLabel.setComponentID ("settings.language.source");
	sourceLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
	sourceLabel.setJustificationType (juce::Justification::centredLeft);
	sourceLabel.setMinimumHorizontalScale (0.7f);
	addAndMakeVisible (sourceLabel);

	detailLabel.setComponentID ("settings.language.detail");
	detailLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
	detailLabel.setJustificationType (juce::Justification::centredLeft);
	detailLabel.setMinimumHorizontalScale (0.75f);
	addAndMakeVisible (detailLabel);

	warningsLabel.setComponentID ("settings.language.warnings");
	warningsLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
	warningsLabel.setJustificationType (juce::Justification::topLeft);
	warningsLabel.setMinimumHorizontalScale (0.75f);
	addAndMakeVisible (warningsLabel);

	openFolderButton.setComponentID ("settings.language.openFolder");
	openFolderButton.onClick = [this] { openFolder(); };
	addAndMakeVisible (openFolderButton);

	rescanButton.setComponentID ("settings.language.rescan");
	rescanButton.onClick = [this] { rescan(); };
	addAndMakeVisible (rescanButton);

	refreshLanguage();
	lookAndFeelChanged();
}

void LanguageSettingsPage::refreshLanguage (void)
{
	openFolderButton.setButtonText (i18n::tr ("settings.language.openFolder"));
	rescanButton.setButtonText (i18n::tr ("settings.language.rescan"));
	languageBox.setTooltip (i18n::tr ("settings.language.selectTooltip"));

	refresh();
	repaint();
}

void LanguageSettingsPage::refresh (void)
{
	rebuildList();

	auto& manager = i18n::LanguageManager::getInstance();
	const auto currentId = manager.getCurrentId();
	const auto current = std::find_if (shownLanguages.begin(), shownLanguages.end(),
									   [&currentId] (const i18n::LanguageInfo& info)
									   {
										   return info.id == currentId;
									   });

	if (current != shownLanguages.end() && ! current->builtIn)
	{
		sourceLabel.setText (i18n::tr ("settings.language.sourceFile",
									   current->file.getFullPathName()),
							 juce::dontSendNotification);
	}
	else
	{
		sourceLabel.setText (i18n::tr ("settings.language.sourceBuiltin", manager.getCurrentName()),
							 juce::dontSendNotification);
	}

	detailLabel.setText (i18n::tr ("settings.language.detail", manager.getCurrentLanguageTag(),
								   juce::String (manager.getCurrentMissingKeyCount())),
						 juce::dontSendNotification);

	juce::StringArray lines;

	if (manager.getSelectionWarning().isNotEmpty())
	{
		lines.add (manager.getSelectionWarning());
	}

	lines.addArray (manager.getWarnings());
	warningsLabel.setText (lines.joinIntoString ("\n"), juce::dontSendNotification);
}

void LanguageSettingsPage::rebuildList (void)
{
	auto& manager = i18n::LanguageManager::getInstance();
	shownLanguages = manager.getAvailable();

	const auto currentId = manager.getCurrentId();
	languageBox.clear (juce::dontSendNotification);

	int selected = 0;

	for (std::size_t i = 0; i < shownLanguages.size(); ++i)
	{
		const auto& info = shownLanguages[i];
		languageBox.addItem (manager.getDisplayName (info), (int) i + 1);

		if (info.id == currentId)
		{
			selected = (int) i + 1;
		}
	}

	languageBox.setSelectedId (selected, juce::dontSendNotification);
}

void LanguageSettingsPage::rescan (void)
{
	i18n::LanguageManager::getInstance().rescan();
	refresh();

	// A file that was just added or fixed is not necessarily the selected one, but the pack in
	// force may still have changed; the store's broadcast is what re-reads every open window.
	toybox::settings::SettingsStore::getInstance().notifyChanged();
}

void LanguageSettingsPage::openFolder (void)
{
	// Creating it here is the point: a user who never made a translation should not have to
	// guess the folder's name before the button works.
	const auto directory = i18n::LanguageManager::externalDirectory();

	if (directory.createDirectory())
	{
		directory.revealToUser();
	}
}

void LanguageSettingsPage::paint (juce::Graphics& g)
{
	paintHeading (g, i18n::tr ("settings.language"),
				  i18n::tr ("settings.language.explanation"), getLocalBounds(),
				  kTwoLineHeadingHeight, 2);
}

void LanguageSettingsPage::resized (void)
{
	auto area = pageContent (getLocalBounds(), kTwoLineHeadingHeight);

	languageBox.setBounds (area.removeFromTop (28).removeFromLeft (300));
	area.removeFromTop (theme::cardGap + 2);
	sourceLabel.setBounds (area.removeFromTop (20));
	detailLabel.setBounds (area.removeFromTop (20));
	area.removeFromTop (4);
	warningsLabel.setBounds (area.removeFromTop (52));

	area.removeFromTop (theme::cardGap);
	auto buttons = area.removeFromTop (kFooterHeight);
	openFolderButton.setBounds (buttons.removeFromLeft (130));
	buttons.removeFromLeft (8);
	rescanButton.setBounds (buttons.removeFromLeft (104));
}

void LanguageSettingsPage::lookAndFeelChanged (void)
{
	sourceLabel.setColour (juce::Label::textColourId, theme::primaryText());
	detailLabel.setColour (juce::Label::textColourId, theme::secondaryText());
	warningsLabel.setColour (juce::Label::textColourId, theme::secondaryText());
}

//==============================================================================
// The MIDI page

void CircleHelpButton::paintButton (juce::Graphics& g, bool isMouseOver, bool isButtonDown)
{
	// A hairline circle and a glyph: no button chrome, because the "?" has to read as the tail of
	// the label it follows rather than as a control of its own.
	const auto bounds = getLocalBounds().toFloat().reduced (1.0f);
	const juce::Colour colour = (isMouseOver || isButtonDown) ? theme::accent() : theme::secondaryText();

	g.setColour (colour.withAlpha (isMouseOver || isButtonDown ? 0.30f : 0.16f));
	g.fillEllipse (bounds);
	g.setColour (colour);
	g.drawEllipse (bounds, 1.0f);

	g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
	g.drawFittedText ("?", getLocalBounds(), juce::Justification::centred, 1);
}

MidiSettingsPage::MidiSettingsPage (void)
{
	bendButton.setComponentID ("settings.midi.bend");
	bendButton.onClick = [this]
	{
		toybox::settings::SettingsStore::getInstance()
			.setMidiPitchBendEnabled (bendButton.getToggleState());
		refresh();
	};
	addAndMakeVisible (bendButton);

	bendHint.setFont (juce::Font (juce::FontOptions (12.5f)));
	bendHint.setJustificationType (juce::Justification::topLeft);
	bendHint.setMinimumHorizontalScale (0.8f);
	addAndMakeVisible (bendHint);

	rangeCaption.setFont (juce::Font (juce::FontOptions (12.5f)));
	rangeCaption.setJustificationType (juce::Justification::centredLeft);
	addAndMakeVisible (rangeCaption);

	rangeSlider.setComponentID ("settings.midi.range");
	rangeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
	rangeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 68, 22);
	rangeSlider.setRange ((double) playback::minPitchBendRange,
						  (double) playback::maxPitchBendRange, 1.0);
	rangeSlider.onValueChange = [this]
	{
		toybox::settings::SettingsStore::getInstance()
			.setMidiPitchBendRange ((int) std::lround (rangeSlider.getValue()));
		refresh();
	};
	addAndMakeVisible (rangeSlider);

	rangeHint.setFont (juce::Font (juce::FontOptions (12.5f)));
	rangeHint.setJustificationType (juce::Justification::topLeft);
	rangeHint.setMinimumHorizontalScale (0.8f);
	addAndMakeVisible (rangeHint);

	rootButton.setComponentID ("settings.midi.sampleRoot");
	rootButton.onClick = [this]
	{
		toybox::settings::SettingsStore::getInstance()
			.setTransposeBySampleRoot (rootButton.getToggleState());
		refresh();
	};
	addAndMakeVisible (rootButton);

	// Two kinds of text: the recommendation stays visible under the toggle (one short line), the
	// reasoning goes into this circle's tooltip.
	rootHelp.setComponentID ("settings.midi.sampleRootHelp");
	addAndMakeVisible (rootHelp);

	rootHint.setFont (juce::Font (juce::FontOptions (12.5f)));
	rootHint.setJustificationType (juce::Justification::topLeft);
	rootHint.setMinimumHorizontalScale (0.8f);
	addAndMakeVisible (rootHint);

	refreshLanguage();
	lookAndFeelChanged();
}

void MidiSettingsPage::refreshLanguage (void)
{
	bendButton.setButtonText (i18n::tr ("settings.midi.bend"));
	bendHint.setText (i18n::tr ("settings.midi.bendHint"), juce::dontSendNotification);
	rangeCaption.setText (i18n::tr ("settings.midi.range"), juce::dontSendNotification);
	rangeSlider.setTextValueSuffix (" " + i18n::tr ("settings.midi.semitones"));
	rangeHint.setText (i18n::tr ("settings.midi.rangeHint"), juce::dontSendNotification);
	rootButton.setButtonText (i18n::tr ("settings.midi.sampleRoot"));
	// The tooltip explains only *why* the switch exists; *which chips it is for* is the line under
	// the toggle, because that is the part someone scanning the page needs.
	rootHelp.setTooltip (i18n::tr ("settings.midi.sampleRootHint"));
	rootHint.setText (i18n::tr ("settings.midi.sampleRootRecommended"), juce::dontSendNotification);
	refresh();
	repaint();
}

void MidiSettingsPage::refresh (void)
{
	auto& store = toybox::settings::SettingsStore::getInstance();
	const bool enabled = store.getMidiPitchBendEnabled();

	bendButton.setToggleState (enabled, juce::dontSendNotification);
	rangeSlider.setValue ((double) store.getMidiPitchBendRange(), juce::dontSendNotification);
	rootButton.setToggleState (store.getTransposeBySampleRoot(), juce::dontSendNotification);

	// With bends off the range is not used by anything, so it is shown greyed rather than hidden:
	// the row keeps its place, and the value is still there when the switch goes back on.
	rangeSlider.setEnabled (enabled);
	rangeCaption.setAlpha (enabled ? 1.0f : 0.5f);
	rangeHint.setAlpha (enabled ? 1.0f : 0.5f);
}

void MidiSettingsPage::paint (juce::Graphics& g)
{
	paintHeading (g, i18n::tr ("settings.midi"),
				  i18n::tr ("settings.midi.explanation"), getLocalBounds(), kTwoLineHeadingHeight, 2);
}

void MidiSettingsPage::resized (void)
{
	auto area = pageContent (getLocalBounds(), kTwoLineHeadingHeight);

	bendButton.setBounds (area.removeFromTop (24).removeFromLeft (300));
	area.removeFromTop (4);
	bendHint.setBounds (area.removeFromTop (34));

	area.removeFromTop (theme::cardGap + 4);

	rangeCaption.setBounds (area.removeFromTop (18));
	area.removeFromTop (4);

	auto rangeRow = area.removeFromTop (kFooterHeight);
	rangeSlider.setBounds (rangeRow.removeFromLeft (juce::jmin (300, rangeRow.getWidth())));

	area.removeFromTop (6);
	rangeHint.setBounds (area.removeFromTop (52));

	area.removeFromTop (theme::cardGap + 4);

	auto rootRow = area.removeFromTop (24);

	// The circle has to sit right after the toggle's own text, so its x comes from the text width
	// rather than from a fixed 300 px box: the box would leave the "?" stranded in the middle of
	// the row. The toggle draws its 20 px check box plus a 6 px gap before the text.
	const juce::Font toggleFont (juce::FontOptions (14.0f));
	const int toggleWidth = 20 + 6
		+ (int) std::ceil (toggleFont.getStringWidthFloat (rootButton.getButtonText()));
	rootButton.setBounds (rootRow.removeFromLeft (juce::jmin (rootRow.getWidth(), toggleWidth)));
	rootRow.removeFromLeft (4);
	rootHelp.setBounds (rootRow.removeFromLeft (16).withSizeKeepingCentre (16, 16));

	area.removeFromTop (2);
	rootHint.setBounds (area.removeFromTop (32));	// room for two lines on a narrow page
}

void MidiSettingsPage::lookAndFeelChanged (void)
{
	bendHint.setColour (juce::Label::textColourId, theme::secondaryText());
	rangeCaption.setColour (juce::Label::textColourId, theme::secondaryText());
	rangeHint.setColour (juce::Label::textColourId, theme::secondaryText());

	// The number field's colours have to be set on the slider itself: the box's label copies them
	// when it is created, and the box is recreated whenever the LookAndFeel changes - so a page
	// that only relied on the LookAndFeel's own defaults drew an empty box here (measured).
	rangeSlider.setColour (juce::Slider::textBoxTextColourId, theme::primaryText());
	// Opaque on purpose: createSliderTextBox() copies this into the editor shown while typing and
	// forces the alpha to 1, so a transparent black would become a black box with dark text.
	rangeSlider.setColour (juce::Slider::textBoxBackgroundColourId, theme::card());
	rangeSlider.setColour (juce::Slider::textBoxOutlineColourId, theme::border());
	rangeSlider.setColour (juce::Slider::textBoxHighlightColourId, theme::accent().withAlpha (0.25f));

	rootHint.setColour (juce::Label::textColourId, theme::secondaryText());
	}

//==============================================================================
// The overlay

SettingsOverlay::SettingsOverlay (void)
{
	setComponentID ("settings");

	// The sidebar. The root is not drawn: its children are the pages, and a root that is visible
	// would be one more thing to click and to collapse - "the root cannot be selected" is then not
	// even a rule to enforce, the row is simply not there.
	tree.setComponentID ("settings.tree");
	tree.setRootItemVisible (false);
	// and no markers either: without them the rows lose their indent as well, which is what a flat
	// list of a few pages wants
	tree.setOpenCloseButtonsVisible (false);

	rootItem = std::make_unique<CategoryItem> ("root", i18n::tr ("settings"), -1, nullptr);
	rootItem->addSubItem (new CategoryItem ("appearance", i18n::tr ("settings.appearance"),
										   appearancePageIndex,
										   [this] (int page) { pages.setCurrentView (page); }));
	rootItem->addSubItem (new CategoryItem ("general", i18n::tr ("settings.general"), generalPageIndex,
										   [this] (int page) { pages.setCurrentView (page); }));
	rootItem->addSubItem (new CategoryItem ("language", i18n::tr ("settings.language"), languagePageIndex,
										   [this] (int page) { pages.setCurrentView (page); }));
	// The MIDI page sits with the other pages rather than in the mixer: the bend range describes
	// the instrument the MIDI goes to, which is not something a per-song strip can own.
	rootItem->addSubItem (new CategoryItem ("midi", i18n::tr ("settings.midi"), midiPageIndex,
										   [this] (int page) { pages.setCurrentView (page); }));

	// Audio only where there is an audio device to choose between: the standalone application says
	// so through Source/Standalone/StandaloneHooks.h, and in a plugin the host owns the device. The
	// row goes in with the page, so the sidebar cannot offer a page that was never created.
	const bool hasAudio = toybox::standalone::available();

	if (hasAudio)
	{
		rootItem->addSubItem (new CategoryItem ("audio", i18n::tr ("settings.audio"), audioPageIndex,
											   [this] (int page) { pages.setCurrentView (page); }));
	}

	tree.setRootItem (rootItem.get());
	addAndMakeVisible (tree);

	// The pages are the window's own page container, so a settings page is written exactly like
	// the play or the mixer page.
	appearancePage = new AppearanceSettingsPage();
	generalPage = new GeneralSettingsPage();
	languagePage = new LanguageSettingsPage();
	midiPage = new MidiSettingsPage();
	appearancePage->setComponentID ("settings.page.appearance");
	generalPage->setComponentID ("settings.page.general");
	languagePage->setComponentID ("settings.page.language");
	midiPage->setComponentID ("settings.page.midi");
	pages.addView (appearancePage);
	pages.addView (generalPage);
	pages.addView (languagePage);
	pages.addView (midiPage);

	if (hasAudio)
	{
		audioPage = new AudioSettingsPage();
		audioPage->setComponentID ("settings.page.audio");
		pages.addView (audioPage);
	}

	// The indices the rows carry and the order the stack holds are two lists that have to agree;
	// a page inserted between two others would otherwise open its neighbour, silently.
	jassert (pages.getNumViews() == (hasAudio ? audioPageIndex : midiPageIndex) + 1);

	addAndMakeVisible (pages);

	closeButton.setComponentID ("settings.close");
	closeButton.onClick = [this] { dismiss(); };
	addAndMakeVisible (closeButton);

	// the store, not the constructor argument: the pages read their own values, and only show
	// what a `setThemeMode()` would have written
	refreshLanguage();
	refreshFromStore();
	lookAndFeelChanged();
}

void SettingsOverlay::refreshLanguage (void)
{
	closeButton.setButtonText (i18n::tr ("common.close"));

	if (auto* root = dynamic_cast<CategoryItem*> (rootItem.get()))
	{
		root->setLabel (i18n::tr ("settings"));
	}

	for (int i = 0; i < rootItem->getNumSubItems(); ++i)
	{
		auto* row = dynamic_cast<CategoryItem*> (rootItem->getSubItem (i));

		if (row == nullptr)
		{
			continue;
		}

		switch (row->getPageIndex())
		{
			case appearancePageIndex:
				row->setLabel (i18n::tr ("settings.appearance"));
				break;
			case generalPageIndex:	row->setLabel (i18n::tr ("settings.general")); break;
			case languagePageIndex:	row->setLabel (i18n::tr ("settings.language")); break;
			case midiPageIndex:		row->setLabel (i18n::tr ("settings.midi")); break;
			case audioPageIndex:	row->setLabel (i18n::tr ("settings.audio")); break;
			default: break;
		}
	}

	pages.refreshLanguage();
	repaint();
}

void SettingsOverlay::refreshFromStore (void)
{
	appearancePage->refresh();
	generalPage->refresh();
	languagePage->refresh();
	midiPage->refresh();

	if (audioPage != nullptr)
	{
		audioPage->refresh();
	}

	// The sidebar shows which page is up. Set here rather than only from a click in the tree,
	// because the pages stay where they were left - and an overlay that comes up with nothing
	// marked in its sidebar looks broken.
	if (auto* item = rootItem->getSubItem (pages.getCurrentViewIndex()))
	{
		item->setSelected (true, true, juce::dontSendNotification);
	}
}

void SettingsOverlay::dismiss (void)
{
	if (onClose != nullptr)
	{
		onClose();
	}
}

juce::Rectangle<int> SettingsOverlay::panelBounds (void) const
{
	const int width = juce::jmax (380, juce::jmin (kPanelWidth, getWidth() - 40));
	const int height = juce::jmax (300, juce::jmin (kPanelHeight, getHeight() - 40));
	return getLocalBounds().withSizeKeepingCentre (width, height);
}

juce::Rectangle<int> SettingsOverlay::bodyBounds (void) const
{
	auto body = panelBounds().reduced (kPanelPadding, kPanelPadding);
	body.removeFromBottom (kFooterHeight + kFooterGap);
	body.removeFromTop (kTitleHeight + kTitleGap);
	return body;
}

juce::Rectangle<int> SettingsOverlay::sidebarBounds (void) const
{
	return bodyBounds().removeFromLeft (kSidebarWidth);
}

juce::Rectangle<int> SettingsOverlay::pageAreaBounds (void) const
{
	auto area = bodyBounds();
	area.removeFromLeft (kSidebarWidth + kSidebarGap);
	return area;
}

juce::Rectangle<int> SettingsOverlay::footerBounds (void) const
{
	return panelBounds().reduced (kPanelPadding, kPanelPadding).removeFromBottom (kFooterHeight);
}

void SettingsOverlay::paint (juce::Graphics& g)
{
	// The scrim, exactly as over the About box: it dims the window, and the area around the panel
	// is the "go back" one that mouseDown() below acts on.
	g.fillAll (juce::Colours::black.withAlpha (0.35f));

	const auto panel = panelBounds().toFloat();
	g.setColour (theme::card());
	g.fillRoundedRectangle (panel, theme::cardRadius);
	g.setColour (theme::cardBorder());
	g.drawRoundedRectangle (panel.reduced (0.5f), theme::cardRadius, 1.0f);

	auto inner = panelBounds().reduced (kPanelPadding, kPanelPadding);
	auto title = inner.removeFromTop (kTitleHeight);
	g.setColour (theme::primaryText());
	g.setFont (juce::Font (juce::FontOptions (15.0f, juce::Font::bold)));
	g.drawText (i18n::tr ("settings"), title, juce::Justification::centredLeft, false);

	// The sidebar is a card of its own, and the one inside the panel is recessed rather than
	// raised: it is the window colour, which is the surface *below* a card in both palettes.
	const auto sidebar = sidebarBounds();
	g.setColour (theme::window());
	g.fillRoundedRectangle (sidebar.toFloat(), theme::buttonRadius);
	g.setColour (theme::cardBorder());
	g.drawRoundedRectangle (sidebar.toFloat().reduced (0.5f), theme::buttonRadius, 1.0f);
}

void SettingsOverlay::resized (void)
{
	// the rows sit inside the rounded card paint() draws
	tree.setBounds (sidebarBounds().reduced (5, 6));
	pages.setBounds (pageAreaBounds());
	closeButton.setBounds (footerBounds().removeFromRight (92));
}

void SettingsOverlay::lookAndFeelChanged (void)
{
	// The tree is the only control here that paints itself from colour IDs of its own, and
	// setColour() stores a copy - so this is where a theme change reaches it. The pages re-apply
	// their own colours in their own lookAndFeelChanged(), and everything else is painted with
	// theme:: colours at paint time.
	tree.setColour (juce::TreeView::backgroundColourId, juce::Colours::transparentBlack);
	tree.setColour (juce::TreeView::linesColourId, theme::cardBorder());
	tree.setColour (juce::TreeView::selectedItemBackgroundColourId, theme::accent().withAlpha (0.16f));
	// the row fills would paint over the sidebar card behind them
	tree.setColour (juce::TreeView::oddItemsColourId, juce::Colours::transparentBlack);
	tree.setColour (juce::TreeView::evenItemsColourId, juce::Colours::transparentBlack);
}

void SettingsOverlay::mouseDown (const juce::MouseEvent& event)
{
	// a click on the panel is the panel's business; one on the scrim around it goes back
	if (! panelBounds().contains (event.getPosition()))
	{
		dismiss();
	}
}

bool SettingsOverlay::keyPressed (const juce::KeyPress& key)
{
	// Escape and nothing else: Ctrl+, belongs to the window (PluginEditor::keyPressed), which owns
	// opening this - the same split as the About box.
	if (key == juce::KeyPress::escapeKey)
	{
		dismiss();
		return true;
	}

	return false;
}

} // namespace toybox::ui
