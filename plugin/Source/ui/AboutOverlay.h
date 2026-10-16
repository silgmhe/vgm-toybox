#pragma once

#include "Theme.h"

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
		closeButton.setColour (juce::TextButton::buttonColourId, theme::accent());
		closeButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
		closeButton.onClick = [this] { dismiss(); };
		addAndMakeVisible (closeButton);

		// The two places this project lives, clickable - a hyperlink only opens the browser when the
		// user clicks it, so nothing here talks to the network on its own.
		//
		// Left-justified like the text above them: a HyperlinkButton centres its text by default,
		// which made these two sit in the middle of a panel whose every other line starts at its
		// left edge. They also name themselves for the console harness.
		for (auto* link : { &repositoryLink, &bilibiliLink })
		{
			link->setColour (juce::HyperlinkButton::textColourId, theme::accent());
			link->setJustificationType (juce::Justification::centredLeft);
			addAndMakeVisible (*link);
		}

		repositoryLink.setComponentID ("about.github");
		bilibiliLink.setComponentID ("about.bilibili");
	}

	/** Called when the box is dismissed - the window hides the overlay. */
	std::function<void()> onClose;

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
		g.drawText ("Version " + juce::String (JucePlugin_VersionString) + "   -   commit " + TOYBOX_GIT_COMMIT,
					inner.removeFromTop (18), juce::Justification::centredLeft, false);
		inner.removeFromTop (10);

		g.setColour (theme::primaryText());
		g.setFont (juce::Font (juce::FontOptions (13.0f)));
		g.drawFittedText ("A chiptune player for VGM, VGZ, S98, DRO and GYM songs.",
						  inner.removeFromTop (18), juce::Justification::centredLeft, 1);
		inner.removeFromTop (10);

		g.setColour (theme::secondaryText());
		g.setFont (juce::Font (juce::FontOptions (12.5f)));
		g.drawFittedText ("by silgmhe - built on VGMPlay and libvgm by Valley Bell and contributors.",
						  inner.removeFromTop (18), juce::Justification::centredLeft, 2);
	}

	void resized (void) override
	{
		auto panel = panelBounds();
		auto footer = panel.reduced (22, 20).removeFromBottom (kFooterHeight);

		repositoryLink.setBounds (footer.removeFromTop (20));
		footer.removeFromTop (2);
		bilibiliLink.setBounds (footer.removeFromTop (20));

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
		const int height = juce::jmin (262, getHeight() - 60);
		return getLocalBounds().withSizeKeepingCentre (juce::jmax (300, width), juce::jmax (238, height));
	}

	void dismiss (void)
	{
		if (onClose != nullptr)
		{
			onClose();
		}
	}

	juce::HyperlinkButton repositoryLink { "GitHub: github.com/silgmhe/vgm-toybox",
										   juce::URL ("https://github.com/silgmhe/vgm-toybox") };
	juce::HyperlinkButton bilibiliLink { "BiliBili: space.bilibili.com/249486555",
										 juce::URL ("https://space.bilibili.com/249486555") };
	juce::TextButton closeButton { "Close" };

	/** The panel's bottom strip: the links above the Close button. */
	static constexpr int kFooterHeight = 78;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AboutOverlay)
};

} // namespace toybox::ui
