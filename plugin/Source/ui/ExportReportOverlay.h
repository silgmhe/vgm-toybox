#pragma once

#include "Theme.h"

#include "i18n/LanguageManager.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace toybox::ui
{

/** What an export did, so the panel that reports it does not read the filesystem back.

	`channelsSeen` is the field that earns its place in a bug report: "0 written" means one thing
	when the snapshot had no channels and another when every channel failed to save, and the
	message that only said "nothing was written" cost two rounds of guessing.
*/
struct ExportReport
{
	int channelsSeen = 0;
	int written = 0;
	int skippedSample = 0;		// rows whose shape is a recording rather than a period
	int failed = 0;
	juce::File folder;

	/** True when the numbers count samples rather than channels.

		Two exports share this overlay because they are the same report to the user - what was
		written, where, and what was not - but "channels in the snapshot" is the wrong sentence over
		a list of DPCM samples, and a heading that says "wavetables" over five sample files is worse.
	*/
	bool samples = false;
};

/**
	The window that says what an export did, drawn inside the window like AboutOverlay.

	Same reason as AboutOverlay.h: an AlertWindow becomes a desktop window that a plugin editor
	cannot reliably show, and its modal state blocks the application even when the box never
	appears. A child component has no peer and no modal state, so nothing can get stuck.

	It is a child of the editor rather than of the page that starts the export, so the whole window
	dims - the same as the settings panel. Dimming only the keyboard card made the message look like
	part of that card, and left the rest of the interface looking live while it was blocked.
*/
class ExportReportOverlay : public juce::Component
{
public:
	ExportReportOverlay (void)
	{
		closeButton.setComponentID ("export.close");
		closeButton.onClick = [this] { dismiss(); };
		addAndMakeVisible (closeButton);
	}

	void setReport (const ExportReport& report)
	{
		shown = report;
		refreshLanguage();
		repaint();
	}

	void refreshLanguage (void)
	{
		closeButton.setButtonText (i18n::tr ("export.close"));
	}

	void paint (juce::Graphics& g) override
	{
		// The scrim: dims what is behind and makes the rest of the window a "click to go back" area.
		g.fillAll (juce::Colours::black.withAlpha (0.35f));

		const auto panel = panelBounds();

		g.setColour (theme::card());
		g.fillRoundedRectangle (panel.toFloat(), theme::buttonRadius);
		g.setColour (theme::cardBorder());
		g.drawRoundedRectangle (panel.toFloat().reduced (0.5f), theme::buttonRadius, 1.0f);

		auto inner = panel.reduced (24, 22);
		inner.removeFromBottom (kFooterHeight);		// the Close button

		g.setColour (theme::primaryText());
		g.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
		g.drawText (i18n::tr (shown.samples ? "export.samples.title" : "export.title"), inner.removeFromTop (26),
					juce::Justification::centredLeft, false);

		inner.removeFromTop (12);

		g.setFont (juce::Font (juce::FontOptions (13.5f)));
		g.drawFittedText (summaryLine(), inner.removeFromTop (20),
						  juce::Justification::centredLeft, 1, 1.0f);

		if (shown.written > 0 && shown.folder.getFullPathName().isNotEmpty())
		{
			inner.removeFromTop (4);

			g.setColour (theme::secondaryText());
			g.setFont (juce::Font (juce::FontOptions (12.5f)));
			g.drawFittedText (shown.folder.getFullPathName(), inner.removeFromTop (34),
							  juce::Justification::topLeft, 2, 1.0f);
		}

		const auto skipped = skippedLine();

		if (skipped.isNotEmpty())
		{
			inner.removeFromTop (8);

			g.setColour (theme::secondaryText());
			g.setFont (juce::Font (juce::FontOptions (12.5f)));
			g.drawFittedText (skipped, inner.removeFromTop (46),
							  juce::Justification::topLeft, 3, 1.0f);
		}

		const auto failed = failedLine();

		if (failed.isNotEmpty())
		{
			inner.removeFromTop (6);

			// The one line here that is a problem rather than a note, so it gets the colour the
			// rest of the interface uses for one.
			g.setColour (theme::error());
			g.setFont (juce::Font (juce::FontOptions (12.5f)));
			g.drawFittedText (failed, inner.removeFromTop (32),
							  juce::Justification::topLeft, 2, 1.0f);
		}

		// What the export actually looked at. Without it a report of zero is ambiguous, and the
		// last two rounds of this were spent asking which of the two zeroes it was.
		if (shown.written == 0)
		{
			inner.removeFromTop (8);

			g.setColour (theme::secondaryText());
			g.setFont (juce::Font (juce::FontOptions (11.5f)));
			g.drawFittedText (i18n::tr (shown.samples ? "export.samples.seen" : "export.seen", juce::String (shown.channelsSeen)),
							  inner.removeFromTop (18), juce::Justification::topLeft, 1, 1.0f);
		}
	}

	void resized (void) override
	{
		const auto panel = panelBounds();
		closeButton.setBounds (panel.getRight() - 24 - 88, panel.getBottom() - 22 - 28, 88, 28);
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

	std::function<void ()> onClose;

private:
	juce::Rectangle<int> panelBounds (void) const
	{
		const int width = juce::jmin (480, getWidth() - 60);
		const int height = juce::jmin (320, getHeight() - 60);
		return getLocalBounds().withSizeKeepingCentre (juce::jmax (320, width), juce::jmax (240, height));
	}

	juce::String summaryLine (void) const
	{
		if (shown.written == 0)
		{
			// Two different failures, two different sentences. The panel receiving nothing and the
			// panel receiving channels that could not be written are not the same bug, and the
			// message that did not say which is why this code was "fixed" twice without effect.
			return i18n::tr (shown.channelsSeen == 0 ? (shown.samples ? "export.samples.none" : "export.noChannels") : "export.none");
		}

		return i18n::tr ("export.written", juce::String (shown.written));
	}

	juce::String skippedLine (void) const
	{
		if (shown.skippedSample == 0)
		{
			return {};
		}

		return i18n::tr ("export.skippedSample", juce::String (shown.skippedSample));
	}

	juce::String failedLine (void) const
	{
		if (shown.failed == 0)
		{
			return {};
		}

		return i18n::tr ("export.failed", juce::String (shown.failed));
	}

	void dismiss (void)
	{
		if (onClose != nullptr)
		{
			onClose();
		}
	}

	juce::TextButton closeButton;

	ExportReport shown;

	/** The strip at the bottom that holds the Close button. */
	static constexpr int kFooterHeight = 52;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExportReportOverlay)
};

} // namespace toybox::ui
