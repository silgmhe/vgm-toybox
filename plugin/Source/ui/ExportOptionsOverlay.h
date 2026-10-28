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

	`bits` is the quantisation applied on the way out. Zero - `kNativeBits` in the exporter - means
	"whatever this chip produces"; anything from one to sixteen forces that depth on every channel.
*/
struct ExportOptions
{
	bool looped = false;
	int seconds = 2;
	int bits = 0;
};

/**
	The window that asks what to export, drawn over the whole window like the settings panel.

	It exists because neither the destination nor the depth can be inferred: the same channel wants
	one period for a wavetable synth and two seconds of looping for a sampler, and a shape the chip
	made at four bits is worth exporting at four bits or at one, depending on what the user is
	after. Remembering the last choice means the window is usually one click.

	The layout is computed once, in resized(), and paint() reads the rectangles it stored. It used
	to recompute the same offsets from hard-coded numbers, and the two drifted: the duration field
	sat on top of the hint text because one of them counted a gap the other did not.
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

		const auto setUpRow = [] (juce::Label& caption, juce::Slider& slider,
								  double low, double high, double step, const juce::String& suffix)
		{
			caption.setFont (juce::Font (juce::FontOptions (12.5f)));
			caption.setJustificationType (juce::Justification::centredLeft);

			slider.setSliderStyle (juce::Slider::LinearHorizontal);
			slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 22);
			slider.setRange (low, high, step);
			slider.textFromValueFunction = [suffix] (double value)
			{
				return juce::String ((int) std::lround (value)) + suffix;
			};
		};

		setUpRow (secondsCaption, secondsSlider, 1.0, 10.0, 1.0, " s");
		secondsSlider.setComponentID ("export.options.seconds");
		secondsSlider.valueFromTextFunction = [this] (const juce::String& text)
		{
			const auto digits = text.retainCharacters ("0123456789");

			return digits.isEmpty () ? secondsSlider.getValue () : digits.getDoubleValue ();
		};
		addAndMakeVisible (secondsCaption);
		addAndMakeVisible (secondsSlider);

		setUpRow (bitsCaption, bitsSlider, 1.0, 16.0, 1.0, " bit");
		bitsSlider.setComponentID ("export.options.bits");
		bitsSlider.valueFromTextFunction = [this] (const juce::String& text)
		{
			const auto digits = text.retainCharacters ("0123456789");

			return digits.isEmpty () ? bitsSlider.getValue () : digits.getDoubleValue ();
		};
		addAndMakeVisible (bitsCaption);
		addAndMakeVisible (bitsSlider);

		nativeBitsButton.setComponentID ("export.options.nativeBits");
		nativeBitsButton.onClick = [this] { refreshEnabled (); };
		addAndMakeVisible (nativeBitsButton);

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
				options.bits = nativeBitsButton.getToggleState ()
								   ? 0
								   : (int) std::lround (bitsSlider.getValue ());

				onConfirm (options);
			}
		};
		addAndMakeVisible (confirmButton);
	}

	/** Fills the controls from the remembered choice. Zero bits means "the chip's own depth". */
	void setOptions (const ExportOptions& options)
	{
		loopedButton.setToggleState (options.looped, juce::dontSendNotification);
		singleButton.setToggleState (! options.looped, juce::dontSendNotification);

		secondsSlider.setValue ((double) options.seconds, juce::dontSendNotification);

		const bool native = options.bits <= 0;

		nativeBitsButton.setToggleState (native, juce::dontSendNotification);
		bitsSlider.setValue ((double) (native ? 4 : options.bits), juce::dontSendNotification);

		refreshEnabled ();
		refreshLanguage ();
	}

	void refreshLanguage (void)
	{
		singleButton.setButtonText (i18n::tr ("export.options.single"));
		loopedButton.setButtonText (i18n::tr ("export.options.looped"));
		secondsCaption.setText (i18n::tr ("export.options.seconds"), juce::dontSendNotification);
		bitsCaption.setText (i18n::tr ("export.options.bits"), juce::dontSendNotification);
		nativeBitsButton.setButtonText (i18n::tr ("export.options.nativeBits"));
		cancelButton.setButtonText (i18n::tr ("export.options.cancel"));
		confirmButton.setButtonText (i18n::tr ("export.options.confirm"));

		// The strings changed, so the two painted blocks have to be redrawn; their rectangles
		// belong to resized() and are not touched here.
		repaint ();
	}

	void paint (juce::Graphics& g) override
	{
		g.fillAll (juce::Colours::black.withAlpha (0.35f));

		const auto panel = panelBounds ();

		g.setColour (theme::card ());
		g.fillRoundedRectangle (panel.toFloat (), theme::buttonRadius);
		g.setColour (theme::cardBorder ());
		g.drawRoundedRectangle (panel.toFloat ().reduced (0.5f), theme::buttonRadius, 1.0f);

		g.setColour (theme::primaryText ());
		g.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
		g.drawText (i18n::tr ("export.options.title"), titleArea,
					juce::Justification::centredLeft, false);

		// Both hint areas come from resized(), so they cannot drift from the controls above them.
		g.setColour (theme::secondaryText ());
		g.setFont (juce::Font (juce::FontOptions (11.5f)));
		g.drawFittedText (i18n::tr ("export.options.hint"), modeHintArea,
						  juce::Justification::topLeft, 3);

		g.drawFittedText (i18n::tr ("export.options.bitsHint"), bitsHintArea,
						  juce::Justification::topLeft, 3);
	}

	void resized (void) override
	{
		const auto panel = panelBounds ();
		auto inner = panel.reduced (24, 22);

		auto footer = inner.removeFromBottom (28);
		inner.removeFromBottom (10);

		titleArea = inner.removeFromTop (26);
		inner.removeFromTop (10);

		singleButton.setBounds (inner.removeFromTop (24));
		inner.removeFromTop (4);
		loopedButton.setBounds (inner.removeFromTop (24));
		inner.removeFromTop (12);

		auto secondsRow = inner.removeFromTop (24);
		secondsCaption.setBounds (secondsRow.removeFromLeft (64));
		secondsSlider.setBounds (secondsRow.removeFromLeft (juce::jmin (300, secondsRow.getWidth ())));
		inner.removeFromTop (8);

		modeHintArea = inner.removeFromTop (34);
		inner.removeFromTop (10);

		auto bitsRow = inner.removeFromTop (24);
		bitsCaption.setBounds (bitsRow.removeFromLeft (64));
		bitsSlider.setBounds (bitsRow.removeFromLeft (juce::jmin (300, bitsRow.getWidth ())));
		inner.removeFromTop (8);

		nativeBitsButton.setBounds (inner.removeFromTop (22));
		inner.removeFromTop (6);

		bitsHintArea = inner.removeFromTop (34);

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
		// The length only means something for a loop, and the depth only means something when it is
		// not being taken from the chip. Both are greyed rather than hidden, so the window does not
		// change shape under the pointer.
		const bool looped = loopedButton.getToggleState ();
		const bool custom = ! nativeBitsButton.getToggleState ();

		secondsSlider.setEnabled (looped);
		secondsCaption.setEnabled (looped);

		bitsSlider.setEnabled (custom);
		bitsCaption.setEnabled (custom);
	}

	juce::Rectangle<int> panelBounds (void) const
	{
		const int width = juce::jmin (500, getWidth () - 60);
		const int height = juce::jmin (440, getHeight () - 60);
		return getLocalBounds ().withSizeKeepingCentre (juce::jmax (340, width), juce::jmax (360, height));
	}

	juce::ToggleButton singleButton;
	juce::ToggleButton loopedButton;
	juce::Label secondsCaption;
	juce::Slider secondsSlider;
	juce::Label bitsCaption;
	juce::Slider bitsSlider;
	juce::ToggleButton nativeBitsButton;
	juce::TextButton cancelButton;
	juce::TextButton confirmButton;

	// Filled by resized(), read by paint(): one layout, computed once.
	juce::Rectangle<int> titleArea;
	juce::Rectangle<int> modeHintArea;
	juce::Rectangle<int> bitsHintArea;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ExportOptionsOverlay)
};

} // namespace toybox::ui
