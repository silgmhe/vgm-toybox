#pragma once

#include "Theme.h"

#include "i18n/LanguageManager.h"

namespace toybox::ui
{

/**
	The Help > About box, drawn inside the window rather than as a dialog of its own.

	It started out as an AlertWindow and froze the application: JUCE created the box as a desktop
	window that never became visible - a 0x0 window with the right title - while its modal state
	blocked every click, which is exactly the shape JUCE's own documentation warns about for plugin
	editors. An overlay cannot do that: it is a child of the window like DropHintOverlay, so there
	is no peer, no modal state and nothing that can get stuck. Clicking outside the panel or
	pressing escape dismisses it, and the application underneath keeps running either way.
*/
class AboutOverlay : public juce::Component
{
public:
	AboutOverlay (void)
	{
		closeButton.setComponentID ("about.close");
		closeButton.onClick = [this] { dismiss(); };
		addAndMakeVisible (closeButton);

		// The two places this project lives. Each row is a plain label and a link: the name in
		// front of the address is just text - only the address is clickable, and only it opens the
		// browser when the user clicks it, so nothing here talks to the network on its own. (Both
		// used to be one HyperlinkButton whose text carried the name, which made "GitHub:" clickable
		// too.)
		//
		// Left-justified like the text above them: a HyperlinkButton centres its text by default,
		// which made these rows the only lines in the panel not starting at its left edge. The
		// labels and links also name themselves for the console harness.
		const auto setUpRow = [this] (juce::Label& name, const juce::String& nameText, const juce::String& nameId,
									  juce::HyperlinkButton& link, const juce::String& address,
									  const juce::String& linkId)
		{
			name.setText (nameText, juce::dontSendNotification);
			name.setComponentID (nameId);
			name.setFont (juce::Font (juce::FontOptions (13.0f)));
			// the label keeps the LookAndFeel's text colour, so a theme change reaches it
			addAndMakeVisible (name);

			link.setButtonText (address);
			link.setURL (juce::URL (address));
			link.setComponentID (linkId);
			// All three arguments on purpose: the second is a bool (resizeToMatchComponentHeight),
			// so passing a Justification there compiles - it converts - and quietly leaves the text
			// on the third parameter's default, which is centred. That is exactly what happened
			// here: the links looked centred because the justification was never set.
			link.setFont (juce::Font (juce::FontOptions (13.0f)), false, juce::Justification::centredLeft);
			// the accent is applied by lookAndFeelChanged() below - a HyperlinkButton has no
			// palette default of its own, so it does have to be set explicitly
			addAndMakeVisible (link);
		};

		setUpRow (githubLabel, i18n::tr ("about.github"), "about.github.label",
				  repositoryLink, "https://github.com/silgmhe/vgm-toybox", "about.github");
		setUpRow (bilibiliLabel, i18n::tr ("about.bilibili"), "about.bilibili.label",
				  bilibiliLink, "https://space.bilibili.com/249486555", "about.bilibili");

		refreshLanguage();
		lookAndFeelChanged();
	}

	void refreshLanguage (void)
	{
		closeButton.setButtonText (i18n::tr ("common.close"));
		githubLabel.setText (i18n::tr ("about.github"), juce::dontSendNotification);
		bilibiliLabel.setText (i18n::tr ("about.bilibili"), juce::dontSendNotification);
		repaint();
	}

	/** Called when the box is dismissed - the window hides the overlay. */
	std::function<void()> onClose;

	/** Re-applies the colours of the controls this overlay owns (see
		PluginEditor::lookAndFeelChanged for why that is needed at all). Everything else in the box
		is painted with theme:: colours, so it follows the palette by itself. */
	void lookAndFeelChanged (void) override
	{
		closeButton.setColour (juce::TextButton::buttonColourId, theme::accent());
		closeButton.setColour (juce::TextButton::textColourOffId, theme::onAccent());
		repositoryLink.setColour (juce::HyperlinkButton::textColourId, theme::accent());
		bilibiliLink.setColour (juce::HyperlinkButton::textColourId, theme::accent());
	}

	void paint (juce::Graphics& g) override
	{
		// the scrim: dims everything behind the panel and makes the whole window a "click to go
		// back" area, the same idea as the drag & drop hint
		g.fillAll (juce::Colours::black.withAlpha (0.35f));

		const auto panel = panelBounds();
		g.setColour (theme::card());
		g.fillRoundedRectangle (panel.toFloat(), theme::buttonRadius);
		g.setColour (theme::cardBorder());
		g.drawRoundedRectangle (panel.toFloat().reduced (0.5f), theme::buttonRadius, 1.0f);

		auto inner = panel.reduced (22, 20);
		inner.removeFromBottom (kFooterHeight);		// the links and the Close button

		g.setColour (theme::primaryText());
		g.setFont (juce::Font (juce::FontOptions (20.0f, juce::Font::bold)));
		g.drawText ("VGMToybox", inner.removeFromTop (26), juce::Justification::centredLeft, false);
		inner.removeFromTop (2);

		g.setColour (theme::secondaryText());
		g.setFont (juce::Font (juce::FontOptions (13.0f)));
		g.drawText (i18n::tr ("about.version", juce::String (JucePlugin_VersionString),
							  juce::String (TOYBOX_GIT_COMMIT)),
					inner.removeFromTop (18), juce::Justification::centredLeft, false);
		inner.removeFromTop (10);

		g.setColour (theme::primaryText());
		g.setFont (juce::Font (juce::FontOptions (13.0f)));
		g.drawFittedText (i18n::tr ("about.description"),
						  inner.removeFromTop (18), juce::Justification::centredLeft, 1, 1.0f);
		inner.removeFromTop (10);

		g.setColour (theme::secondaryText());
		g.setFont (juce::Font (juce::FontOptions (12.5f)));
		g.drawFittedText (i18n::tr ("about.credit"),
						  inner.removeFromTop (34), juce::Justification::centredLeft, 2, 1.0f);
		inner.removeFromTop (10);

		g.setColour (theme::secondaryText());
		g.setFont (juce::Font (juce::FontOptions (12.5f)));
		g.drawText (i18n::tr ("about.license"), inner.removeFromTop (18),
					juce::Justification::centredLeft, false);
	}

	void resized (void) override
	{
		auto panel = panelBounds();
		auto footer = panel.reduced (22, 20).removeFromBottom (kFooterHeight);

		// the name column is wide enough for the longer of the two ("BiliBili: "), so the addresses
		// start on one line with each other - and the rows start where the text above them does
		constexpr int kNameWidth = 64;

		const auto githubRow = footer.removeFromTop (20);
		githubLabel.setBounds (githubRow.withWidth (kNameWidth));
		repositoryLink.setBounds (githubRow.withTrimmedLeft (kNameWidth));

		footer.removeFromTop (2);

		const auto bilibiliRow = footer.removeFromTop (20);
		bilibiliLabel.setBounds (bilibiliRow.withWidth (kNameWidth));
		bilibiliLink.setBounds (bilibiliRow.withTrimmedLeft (kNameWidth));

		closeButton.setBounds (panel.getRight() - 22 - 88, panel.getBottom() - 20 - 28, 88, 28);
	}

	void mouseDown (const juce::MouseEvent& event) override
	{
		if (! panelBounds().contains (event.getPosition()))
		{
			dismiss();
		}
	}

	bool keyPressed (const juce::KeyPress& key) override
	{
		if (key == juce::KeyPress::escapeKey)
		{
			dismiss();
			return true;
		}

		return false;
	}

private:
	juce::Rectangle<int> panelBounds (void) const
	{
		const int width = juce::jmin (470, getWidth() - 60);
		const int height = juce::jmin (280, getHeight() - 60);
		return getLocalBounds().withSizeKeepingCentre (juce::jmax (300, width), juce::jmax (256, height));
	}

	void dismiss (void)
	{
		if (onClose != nullptr)
		{
			onClose();
		}
	}

	// each row is two things: the name is plain text, only the address is a link
	juce::Label githubLabel;
	juce::HyperlinkButton repositoryLink;
	juce::Label bilibiliLabel;
	juce::HyperlinkButton bilibiliLink;
	juce::TextButton closeButton;

	/** The panel's bottom strip: the links above the Close button. */
	static constexpr int kFooterHeight = 78;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AboutOverlay)
};

} // namespace toybox::ui
