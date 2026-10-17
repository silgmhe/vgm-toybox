#include "SettingsOverlay.h"

namespace toybox::ui
{

namespace
{
	// The panel, and how it divides: a title strip, a sidebar next to the pages, a footer with the
	// one way out that is always in the same place.
	constexpr int kPanelWidth = 640;
	constexpr int kPanelHeight = 430;
	constexpr int kPanelPadding = 18;
	constexpr int kTitleHeight = 22;
	constexpr int kTitleGap = 12;
	constexpr int kSidebarWidth = 150;
	constexpr int kSidebarGap = 14;
	constexpr int kFooterGap = 12;
	constexpr int kFooterHeight = 32;		// a button face is 28: the bounds are face + 2*inset
	constexpr int kHeadingHeight = 46;
	constexpr int kThemeChoiceGroup = 1;	// one radio group per page, so any number will do

	/** The heading both pages start with: what the page is, then what it does. */
	void paintHeading (juce::Graphics& g, const juce::String& title, const juce::String& explanation,
					   juce::Rectangle<int> bounds)
	{
		auto area = bounds.reduced (kPanelPadding, 0).removeFromTop (kHeadingHeight);
		auto titleArea = area.removeFromTop (24);

		g.setColour (theme::primaryText());
		g.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
		g.drawText (title, titleArea, juce::Justification::centredLeft, false);

		g.setColour (theme::secondaryText());
		g.setFont (juce::Font (juce::FontOptions (12.5f)));
		g.drawFittedText (explanation, area, juce::Justification::centredLeft, 1);
	}

	/** Where the controls go: under the heading, both pages starting at the same place. */
	juce::Rectangle<int> pageContent (juce::Rectangle<int> bounds)
	{
		auto area = bounds.reduced (kPanelPadding, 0);
		area.removeFromTop (kHeadingHeight);
		return area;
	}

	/**
		One row of the sidebar.

		`page` is the page it shows, and -1 for the root: the root is a heading rather than a choice,
		and `TreeViewItem::canBeSelected()` is the switch that says so. It is never drawn at all
		(see `setRootItemVisible` below) - it exists to hold the rows, so a third settings page is
		one `addSubItem` and nothing else.
	*/
	class CategoryItem : public juce::TreeViewItem
	{
	public:
		CategoryItem (const juce::String& nameToShow, int pageIndex, std::function<void (int)> onPicked)
			: name (nameToShow), page (pageIndex), picked (std::move (onPicked))
		{
		}

		bool canBeSelected (void) const override { return page >= 0; }
		bool mightContainSubItems (void) override { return page < 0; }
		int getItemHeight (void) const override { return 26; }
		juce::String getUniqueName (void) const override { return "settings.category." + name; }

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
		juce::String name;
		int page;
		std::function<void (int)> picked;
	};
}

//==============================================================================
// Theme

ThemeSettingsPage::ThemeSettingsPage (void)
{
	lightButton.setComponentID ("settings.theme.light");
	darkButton.setComponentID ("settings.theme.dark");
	followButton.setComponentID ("settings.theme.followSystem");
	currentLabel.setComponentID ("settings.theme.current");

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

	currentLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
	currentLabel.setJustificationType (juce::Justification::centredLeft);

	addAndMakeVisible (lightButton);
	addAndMakeVisible (darkButton);
	addAndMakeVisible (followButton);
	addAndMakeVisible (currentLabel);

	refresh();
	lookAndFeelChanged();
}

void ThemeSettingsPage::select (toybox::settings::SettingsStore::ThemeMode mode)
{
	toybox::settings::SettingsStore::getInstance().setThemeMode (mode);

	// The refresh is here and not in a change listener because a click on the option that is
	// already on changes nothing in the store (SettingsStore.cpp:106-109) - it broadcasts nothing,
	// and the page still has to be left showing what is stored rather than what was clicked.
	refresh();
}

void ThemeSettingsPage::refresh (void)
{
	const auto mode = toybox::settings::SettingsStore::getInstance().getThemeMode();

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
}

juce::String ThemeSettingsPage::describeFollowSystem (void) const
{
	const juce::String effective (theme::systemTheme() == theme::Id::dark ? "Dark" : "Light");
	return "Follow system (now: " + effective + ")";
}

void ThemeSettingsPage::paint (juce::Graphics& g)
{
	paintHeading (g, "Theme", "Which colours the window draws with. A change applies at once.", getLocalBounds());
}

void ThemeSettingsPage::resized (void)
{
	auto area = pageContent (getLocalBounds());

	auto choices = area.removeFromTop (kFooterHeight);
	lightButton.setBounds (choices.removeFromLeft (96));
	choices.removeFromLeft (theme::rowGap);
	darkButton.setBounds (choices.removeFromLeft (96));

	area.removeFromTop (theme::cardGap + 4);
	followButton.setBounds (area.removeFromTop (24).removeFromLeft (220));

	area.removeFromTop (4);
	currentLabel.setBounds (area.removeFromTop (18));
}

void ThemeSettingsPage::lookAndFeelChanged (void)
{
	// The one colour this page sets on a control it owns: the options and the checkbox take the
	// LookAndFeel's own (card face, accent while on, primary text), which is the point of them
	// being LNF-coloured at all - see PluginEditor::lookAndFeelChanged.
	currentLabel.setColour (juce::Label::textColourId, theme::secondaryText());
}

//==============================================================================
// General

GeneralSettingsPage::GeneralSettingsPage (void)
{
	fileCaption.setComponentID ("settings.general.pathCaption");
	fileCaption.setText ("Settings file", juce::dontSendNotification);
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

	refresh();
	lookAndFeelChanged();
}

void GeneralSettingsPage::refresh (void)
{
	auto& store = toybox::settings::SettingsStore::getInstance();

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
	paintHeading (g, "General", "Where these settings live, and how to put them back.", getLocalBounds());
}

void GeneralSettingsPage::resized (void)
{
	auto area = pageContent (getLocalBounds());

	fileCaption.setBounds (area.removeFromTop (16));
	pathLabel.setBounds (area.removeFromTop (20));

	area.removeFromTop (theme::cardGap + 6);
	auto buttons = area.removeFromTop (kFooterHeight);
	openFolderButton.setBounds (buttons.removeFromLeft (112));
	buttons.removeFromLeft (theme::rowGap);
	resetButton.setBounds (buttons.removeFromLeft (146));

	area.removeFromTop (theme::cardGap + 6);
	errorLabel.setBounds (area.removeFromTop (18));
}

void GeneralSettingsPage::lookAndFeelChanged (void)
{
	// The caption is an explanation and the error line is a warning; the path itself is read, so
	// it takes the LookAndFeel's primary text colour and needs nothing said about it here.
	fileCaption.setColour (juce::Label::textColourId, theme::secondaryText());
	errorLabel.setColour (juce::Label::textColourId, theme::error());
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
	// list of two pages wants
	tree.setOpenCloseButtonsVisible (false);

	rootItem = std::make_unique<CategoryItem> ("Settings", -1, nullptr);
	rootItem->addSubItem (new CategoryItem ("Theme", 0, [this] (int page) { pages.setCurrentView (page); }));
	rootItem->addSubItem (new CategoryItem ("General", 1, [this] (int page) { pages.setCurrentView (page); }));
	tree.setRootItem (rootItem.get());
	addAndMakeVisible (tree);

	// The pages are the window's own page container, so a settings page is written exactly like
	// the play or the mixer page.
	themePage = new ThemeSettingsPage();
	generalPage = new GeneralSettingsPage();
	themePage->setComponentID ("settings.page.theme");
	generalPage->setComponentID ("settings.page.general");
	pages.addView (themePage);
	pages.addView (generalPage);
	addAndMakeVisible (pages);

	closeButton.setComponentID ("settings.close");
	closeButton.onClick = [this] { dismiss(); };
	addAndMakeVisible (closeButton);

	// the store, not the constructor argument: the pages read their own values, and only show
	// what a `setThemeMode()` would have written
	refreshFromStore();
	lookAndFeelChanged();
}

void SettingsOverlay::refreshFromStore (void)
{
	themePage->refresh();
	generalPage->refresh();

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
	g.drawText ("Settings", title, juce::Justification::centredLeft, false);

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
