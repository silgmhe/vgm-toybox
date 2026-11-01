#pragma once

#include "Theme.h"

#include "i18n/LanguageManager.h"

namespace toybox::ui
{

/**
	A titled message with one way out, drawn inside the window.

	The same reasoning as AboutOverlay applies, and it is worth repeating because this is the
	component people reach for when they want a dialog: JUCE's AlertWindow froze this application
	once - a 0x0 window that never became visible while its modal state blocked every click. An
	overlay is a child of the window, so there is no peer, no modal state and nothing that can get
	stuck. Clicking outside the panel or pressing escape dismisses it.

	It does exactly one thing: a title, a body and a button. Not a second button, not a return
	value, not an icon - ExportReportOverlay showed where that road goes, and the more an overlay
	can express, the more logic it collects.
*/
class MessageBoxOverlay : public juce::Component
{
public:
	struct Message
	{
		juce::String title;
		juce::String body;
		juce::String dismiss;		// empty means common.ok
	};

	MessageBoxOverlay (void)
	{
		closeButton.setComponentID ("messagebox.close");
		closeButton.onClick = [this] { dismiss(); };
		addAndMakeVisible (closeButton);

		refreshLanguage();
		lookAndFeelChanged();
	}

	/** Shows `message`. Callers set this right before making the overlay visible. */
	void setMessage (Message message)
	{
		shown = std::move (message);
		refreshLanguage();
		repaint();
	}

	void refreshLanguage (void)
	{
		closeButton.setButtonText (shown.dismiss.isNotEmpty() ? shown.dismiss
															  : i18n::tr ("common.ok"));
	}

	/** Called when the box is dismissed - the window hides the overlay. */
	std::function<void()> onDismiss;

	void lookAndFeelChanged (void) override
	{
		// The one colour this overlay sets on a control it owns; everything else is painted with
		// theme:: colours and follows the palette by itself.
		closeButton.setColour (juce::TextButton::buttonColourId, theme::accent());
		closeButton.setColour (juce::TextButton::textColourOffId, theme::onAccent());
	}

	void paint (juce::Graphics& g) override
	{
		// The scrim is a palette colour, not a literal: "the backdrop behind a modal" is the same
		// gesture as a window shade, so it has one definition (see Theme.h).
		g.fillAll (theme::scrim());

		const auto panel = panelBounds();
		g.setColour (theme::card());
		g.fillRoundedRectangle (panel.toFloat(), theme::buttonRadius);
		g.setColour (theme::cardBorder());
		g.drawRoundedRectangle (panel.toFloat().reduced (0.5f), theme::buttonRadius, 1.0f);

		auto inner = panel.reduced (22, 20);
		inner.removeFromBottom (kFooterHeight);

		g.setColour (theme::primaryText());
		g.setFont (juce::Font (juce::FontOptions (17.0f, juce::Font::bold)));
		g.drawFittedText (shown.title, inner.removeFromTop (24), juce::Justification::centredLeft, 1, 1.0f);

		inner.removeFromTop (8);

		g.setColour (theme::secondaryText());
		g.setFont (juce::Font (juce::FontOptions (13.0f)));
		g.drawFittedText (shown.body, inner, juce::Justification::topLeft, kBodyLines, 1.0f);
	}

	void resized (void) override
	{
		const auto panel = panelBounds();
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
		// Wide enough for a sentence and short enough that it does not become a page: a message
		// that needs more than kBodyLines is one that wants a document, not a box.
		const int width = juce::jmin (430, getWidth() - 60);
		const int height = juce::jmin (180, getHeight() - 60);
		return getLocalBounds().withSizeKeepingCentre (juce::jmax (300, width), juce::jmax (160, height));
	}

	void dismiss (void)
	{
		if (onDismiss != nullptr)
		{
			onDismiss();
		}
	}

	Message shown;

	juce::TextButton closeButton;

	static constexpr int kFooterHeight = 44;
	static constexpr int kBodyLines = 4;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MessageBoxOverlay)
};

} // namespace toybox::ui
