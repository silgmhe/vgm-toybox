#include "ChannelStrip.h"

#include "Theme.h"

namespace vgmplay::ui
{
namespace
{
	constexpr int kNameHeight = 13;
	constexpr int kKnobHeight = 30;
	constexpr int kButtonHeight = 24;	// the M/S buttons share the full strip width
	constexpr int kButtonGap = 6;
	constexpr int kFaderGap = 4;
	constexpr int kMeterWidth = 10;		// column: two 3 px bars with a 2 px gap
}

ChannelStrip::ChannelStrip (void)
{
	// the slider colours come from LightLookAndFeel (accent track, white handle)
	gainSlider.setRange (0.0, 2.0, 0.01);
	gainSlider.setDoubleClickReturnValue (true, 1.0);
	gainSlider.setTooltip ("Gain (double-click: 100%)");
	gainSlider.onValueChange = [this]
	{
		if (onGainChanged != nullptr)
		{
			onGainChanged ((float) gainSlider.getValue());
		}
	};

	panKnob.setRange (-1.0, 1.0, 0.01);
	panKnob.setDoubleClickReturnValue (true, 0.0);
	panKnob.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
								 juce::MathConstants<float>::pi * 2.75f, true);
	panKnob.setTooltip ("Pan (double-click: centre)");
	panKnob.onValueChange = [this]
	{
		if (onPanChanged != nullptr)
		{
			onPanChanged ((float) panKnob.getValue());
		}
	};

	// DAW convention: mute is red, solo is amber
	muteButton.setClickingTogglesState (true);
	muteButton.setColour (juce::TextButton::buttonOnColourId, theme::mute());
	muteButton.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
	muteButton.setTooltip ("Mute this track");
	muteButton.onClick = [this]
	{
		if (onMuteChanged != nullptr)
		{
			onMuteChanged (muteButton.getToggleState());
		}
	};

	soloButton.setClickingTogglesState (true);
	soloButton.setColour (juce::TextButton::buttonOnColourId, theme::solo());
	soloButton.setColour (juce::TextButton::textColourOnId, theme::primaryText());
	soloButton.setTooltip ("Solo this track (silences every track that is not soloed)");
	soloButton.onClick = [this]
	{
		if (onSoloChanged != nullptr)
		{
			onSoloChanged (soloButton.getToggleState());
		}
	};

	addAndMakeVisible (gainSlider);
	addAndMakeVisible (panKnob);
	addAndMakeVisible (muteButton);
	addAndMakeVisible (soloButton);
}

void ChannelStrip::setup (const juce::String& name, bool isMasterStrip)
{
	stripName = name;
	master = isMasterStrip;

	panKnob.setVisible (! master);
	muteButton.setVisible (! master);
	soloButton.setVisible (! master);
	gainSlider.setTooltip (master ? "Master volume (double-click: 100%)" : "Gain (double-click: 100%)");

	resized();
	repaint();
}

void ChannelStrip::setValues (float gain, float pan, bool muted, bool soloed,
							  float peakLValue, float peakRValue, float dim)
{
	gainSlider.setValue (gain, juce::dontSendNotification);
	panKnob.setValue (pan, juce::dontSendNotification);
	muteButton.setToggleState (muted, juce::dontSendNotification);
	soloButton.setToggleState (soloed, juce::dontSendNotification);

	peakL = peakLValue;
	peakR = peakRValue;

	setAlpha (dim);		// dims the strip (and its controls) while its chip is muted
	repaint();			// the meters changed
}

void ChannelStrip::resized (void)
{
	auto inner = getLocalBounds().reduced (6, 6);
	nameBounds = inner.removeFromTop (kNameHeight);
	inner.removeFromTop (3);

	if (master)
	{
		inner.removeFromTop (2);
	}
	else
	{
		panKnob.setBounds (inner.removeFromTop (kKnobHeight));
		inner.removeFromTop (kFaderGap);
	}

	if (! master)
	{
		// M and S split the strip width, like the buttons of a hardware console
		auto buttons = inner.removeFromBottom (kButtonHeight);
		const int buttonWidth = (buttons.getWidth() - kButtonGap) / 2;
		muteButton.setBounds (buttons.removeFromLeft (buttonWidth));
		buttons.removeFromLeft (kButtonGap);
		soloButton.setBounds (buttons.removeFromLeft (buttonWidth));
		inner.removeFromBottom (6);
	}

	// the meter column sits left of the fader, both filling the remaining height
	auto meterColumn = inner.removeFromLeft (kMeterWidth);
	meterBounds = meterColumn.withSizeKeepingCentre (8, meterColumn.getHeight());
	inner.removeFromLeft (kFaderGap);
	gainSlider.setBounds (inner);
}

void ChannelStrip::paint (juce::Graphics& g)
{
	const auto area = getLocalBounds().toFloat();
	g.setColour (theme::track().withAlpha (0.30f));
	g.fillRoundedRectangle (area, theme::buttonRadius);
	g.setColour (theme::cardBorder());
	g.drawRoundedRectangle (area.reduced (0.5f), theme::buttonRadius, 1.0f);

	g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
	g.setColour (theme::primaryText());
	g.drawText (stripName, nameBounds, juce::Justification::centred, true);

	drawMeter (g);
}

void ChannelStrip::drawMeter (juce::Graphics& g) const
{
	auto bars = meterBounds;
	const auto drawBar = [] (juce::Graphics& graphics, juce::Rectangle<int> bar, float level)
	{
		graphics.setColour (theme::track());
		graphics.fillRoundedRectangle (bar.toFloat(), 1.5f);

		const float clamped = juce::jlimit (0.0f, 1.0f, level);
		const float filled = (float) bar.getHeight() * clamped;
		if (filled >= 1.0f)
		{
			graphics.setColour (clamped >= 0.99f ? theme::error() : theme::accent());
			graphics.fillRoundedRectangle (bar.toFloat().withTop ((float) bar.getBottom() - filled), 1.5f);
		}
	};

	const int barWidth = juce::jmax (2, (bars.getWidth() - 2) / 2);
	drawBar (g, bars.removeFromLeft (barWidth), peakL);
	bars.removeFromLeft (2);
	drawBar (g, bars.removeFromLeft (barWidth), peakR);
}

} // namespace vgmplay::ui
