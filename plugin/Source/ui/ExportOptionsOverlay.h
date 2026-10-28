#pragma once

#include "Theme.h"

#include "i18n/LanguageManager.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace toybox::ui
{

/** What the user chose before an export started.

	`looped` is not a format but a destination: one period is what a wavetable synth reads, and a
	loop at concert pitch is what a sampler reads. They are different files of different lengths at
	different rates, so the choice has to be made before anything is written.
*/
struct ExportOptions
{
	bool looped = false;
	int seconds = 2;
};

/**
	The window that asks what to export, drawn over the whole window like the settings panel.

	It exists because the two destinations cannot be inferred: the same channel wants one period for
	Serum and two seconds of looping for a sampler, and nothing in the snapshot says which the user
	means. Remembering the last choice in the settings store means the window is usually one click.
*/
class ExportOptionsOverlay : public juce::Component
{
public:
	ExportOptionsOverlay (void)
	{
		singleButton.setComponentID ("export.options.single");
		singleButton.setRadioGroupId (1);
		singleButton.onClick = [this] { refreshEnabled (); };
		addAndMakeVisible (singleButton);

		loopedButton.setComponentID ("export.options.looped");
		loopedButton.setRadioGroupId (1);
		loopedButton.onClick = [this] { refreshEnabled (); };
		addAndMakeVisible (loopedButton);

		secondsCaption.setFont (juce::Font (juce::FontOptions (12.5f)));
		secondsCaption.setJustificationType (juce::Justification::centredLeft);
		addAndMakeVisible (secondsCaption);

		secondsSlider.setComponentID ("export.options.seconds");
		secondsSlider.setSliderStyle (juce::Slider::LinearHorizontal);
		secondsSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 22);
		secondsSlider.setRange (1.0, 10.0, 1.0);
		secondsSlider.textFromValueFunction = [] (double value)
		{
			return juce::String ((int) std::lround (value)) + " s";
		};
		secondsSlider.valueFromTextFunction = [this] (const juce::String& text)
		{
			const auto digits = text.retainCharacters ("0123456789");

			return digits.isEmpty () ? secondsSlider.getValue () : digits.getDoubleValue ();
		};
		addAndMakeVisible (secondsSlider);

		cancelButton.setComponentID ("export.options.cancel");
		cancelButton.onClick = [this] { if (onCancel != nullptr) { onCancel (); } };
		addAndMakeVisible (cancelButton);

		confirmButton.setComponentID ("export.options.confirm");
		confirmButton.onClick = [this]
		{
			if (onConfirm != nullptr)
			{
				ExportOptions options;
				options.looped = loopedButton.getToggleState ();
				options.seconds = (int) std::lround (secondsSlider.getValue ());
				onConfirm (options);
			}
		};
		addAndMakeVisible (confirmButton);
	}

	/** Fills the controls from the remembered choice. */
	void setOptions (const ExportOptions& options)
	{
		loopedButton.setToggleState (options.looped, juce::dontSendNotification);
		singleButton.setToggleState (! options.looped, juce::dontSendNotification);
		secondsSlider.setValue ((double) options.seconds, juce::dontSendNotification);

		refreshEnabled ();
		refreshLanguage ();
	}

	void refreshLanguage (void)
	{
		singleButton.setButtonText (i18n::tr ("export.options.single"));
		loopedButton.setButtonText (i18n::tr ("export.options.looped"));
		secondsCaption.setText (i18n::tr ("export.options.seconds"), juce::dontSendNotification);
		cancelButton.setButtonText (i18n::tr ("export.options.cancel"));
		confirmButton.setButtonText (i18n::tr ("export.options.confirm"));
	}

	void paint (juce::Graphics& g) override
	{
		g.fillAll (juce::Colours::black.withAlpha (0.35f));

		const auto panel = panelBounds ();

		g.setColour (theme::card ());
		g.fillRoundedRectangle (panel.toFloat (), theme::buttonRadius);
		g.setColour (theme::cardBorder ());
		g.drawRoundedRectangle (panel.toFloat ().reduced (0.5f), theme::buttonRadius, 1.0f);

		auto inner = panel.reduced (24, 22);

		g.setColour (theme::primaryText ());
		g.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
		g.drawText (i18n::tr ("export.options.title"), inner.removeFromTop (26),
					juce::Justification::centredLeft, false);

		// The two sentences that say which destination each choice is for. A user who has never
		// imported a wavetable has no way to tell "single cycle" from "looped" otherwise.
		inner.removeFromTop (26 + 6 + 26 + 10);

		g.setColour (theme::secondaryText ());
		g.setFont (juce::Font (juce::FontOptions (11.5f)));
		g.drawFittedText (i18n::tr ("export.options.hint"), inner.removeFromTop (44),
						  juce::Justification::topLeft, 3);
	}

	void resized (void) override
	{
		const auto panel = panelBounds ();
		auto inner = panel.reduced (24, 22);

		inner.removeFromTop (26);			// title
		inner.removeFromTop (8);

		singleButton.setBounds (inner.removeFromTop (26));
		inner.removeFromTop (4);
		loopedButton.setBounds (inner.removeFromTop (26));

		inner.removeFromTop (10);

		auto secondsRow = inner.removeFromTop (26);
		secondsCaption.setBounds (secondsRow.removeFromLeft (80));
		secondsSlider.setBounds (secondsRow.removeFromLeft (juce::jmin (260, secondsRow.getWidth ())));

		inner = panel.reduced (24, 22);
		auto footer = panel.reduced (24, 22).removeFromBottom (28);

		confirmButton.setBounds (footer.removeFromRight (88));
		footer.removeFromRight (8);
		cancelButton.setBounds (footer.removeFromRight (88));
	}

	void mouseDown (const juce::MouseEvent& event) override
	{
		if (! panelBounds ().contains (event.getPosition ()) && onCancel != nullptr)
		{
			onCancel ();
		}
	}

	bool keyPressed (const juce::KeyPress& key) override
	{
		if (key == juce::KeyPress::escapeKey && onCancel != nullptr)
		{
			onCancel ();
			return true;
		}

		return false;
	}

	std::function<void (const ExportOptions&)> onConfirm;
	std::function<void ()> onCancel;

private:
	void refreshEnabled (void)
	{
		// The length only means something for a loop; a single period is one period at any
		// duration. Greyed rather than hidden, so the window does not change shape between modes.
		secondsSlider.setEnabled (loopedButton.getToggleState ());
		secondsCaption.setEnabled (loopedButton.getToggleState ());
	}

	juce::Rectangle<int> panelBounds (void) const
	{
		const int width = juce::jmin (480, getWidth () - 60);
		const int height = juce::jmin (340, getHeight () - 60);
		return getLocalBounds ().withSizeKeepingCentre (juce::jmax (320, width), juce::jmax (280, height));
	}

	juce::ToggleButton singleButton;
	juce::ToggleButton loopedButton;
	juce::Label secondsCaption;
	juce::Slider secondsSlider;
	juce::TextButton cancelButton;
	juce::TextButton confirmButton;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExportOptionsOverlay)
};

} // namespace toybox::ui
