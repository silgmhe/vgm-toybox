#include "ChannelStrip.h"

#include <cmath>

#include "MeterScale.h"
#include "Theme.h"

namespace vgmplay::ui
{
namespace
{
	constexpr int kNameHeight = 13;
	constexpr int kKnobHeight = 30;
	constexpr int kButtonHeight = 24;	// the M/S buttons share the full strip width
	constexpr int kButtonGap = 6;
	constexpr int kOutputHeight = 22;	// the output selector sits at the very bottom
	constexpr int kOutputGap = 6;
	constexpr int kColumnGap = 3;
	constexpr int kMeterWidth = 15;		// two 6 px bars with a 3 px gap
	constexpr int kMeterGap = 3;
	constexpr int kScaleWidth = 16;		// graduation line + its tiny label
}

ChannelStrip::ChannelStrip (void)
{
	// the slider colours come from LightLookAndFeel (accent track, white handle)
	gainSlider.setNormalisableRange (meter::gainRange());	// dB-linear travel, linear gain value
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

	// the output selector: single choice (Master = send to the internal master bus, or a real
	// output bus = direct out), filled by MixerPanel through setOutputOptions()
	outputBox.setTooltip ("Output bus");
	outputBox.onChange = [this]
	{
		const int index = outputBox.getSelectedId() - 1;
		if (index >= 0 && index < (int) outputOptions.size() && onOutputChanged != nullptr)
		{
			onOutputChanged (outputOptions[(size_t) index].target);
		}
	};
	addAndMakeVisible (outputBox);

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
	soloButton.setVisible (! master);

	// the master strip has no solo, but its mute silences what the master bus contributes to
	// its own target bus (tracks sent to real buses keep playing - see setMasterMuted)
	muteButton.setVisible (true);
	muteButton.setTooltip (master ? "Mute the master bus output"
								  : "Mute this track");
	outputBox.setTooltip (master ? "Output bus - where the master mix goes"
								 : "Output bus - where this track goes");
	gainSlider.setTooltip (master ? "Mixer master gain - the output volume sits behind it "
									"(double-click: 100%)"
								  : "Gain (double-click: 100%)");

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

	// Only the meter area is repainted, and only when it actually moved: repainting the strip
	// would drag its child controls into every frame, and each of those draws a blurred
	// shadow - that is what makes the difference between a quiet mixer and a busy GPU.
	const bool meterMoved = (std::abs (peakL - drawnPeakL) > 0.001f
							 || std::abs (peakR - drawnPeakR) > 0.001f);
	if (meterMoved)
	{
		drawnPeakL = peakL;
		drawnPeakR = peakR;
		repaint (meterBounds.isEmpty() ? getLocalBounds() : meterBounds.expanded (1));
	}
}

void ChannelStrip::setOutputOptions (const std::vector<OutputOption>& options, playback::OutputTarget selected)
{
	outputOptions = options;
	shownOutput = selected;

	outputBox.clear (juce::dontSendNotification);

	bool selectedSomething = false;
	for (size_t i = 0; i < outputOptions.size(); ++i)
	{
		const OutputOption& option = outputOptions[i];
		const int itemId = (int) i + 1;

		// a bus the host has not connected is still selectable - the track routed to it is
		// silent, and the label says why (docs/multitrack-output-design.md, R6/E)
		outputBox.addItem (option.connected ? option.name : option.name + " (not connected)", itemId);

		if (option.target == selected)
		{
			outputBox.setSelectedId (itemId, juce::dontSendNotification);
			selectedSomething = true;
		}
	}

	if (! selectedSomething && outputBox.getNumItems() > 0)
	{
		outputBox.setSelectedId (1, juce::dontSendNotification);
	}
}

void ChannelStrip::setMixerActive (bool active)
{
	// the routing is inactive while the mixer is off: the selector stays visible (the values are
	// kept) but greyed out
	outputBox.setEnabled (active);
}

void ChannelStrip::resized (void)
{
	auto inner = getLocalBounds().reduced (6, 6);
	nameBounds = inner.removeFromTop (kNameHeight);
	inner.removeFromTop (3);

	// the output selector takes the bottom row; the M/S row (or the master's reserved space
	// for it) sits directly above, so the fader and graduations stay aligned between strips
	outputBox.setBounds (inner.removeFromBottom (kOutputHeight));
	inner.removeFromBottom (kOutputGap);

	if (master)
	{
		// The master strip has no knob and no buttons, but it keeps their space: its fader and
		// graduations then line up with the track strips instead of being twice as long.
		inner.removeFromTop (kKnobHeight + 4);
	}
	else
	{
		panKnob.setBounds (inner.removeFromTop (kKnobHeight));
		inner.removeFromTop (4);
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
	else
	{
		inner.removeFromBottom (kButtonHeight + 6);
	}

	// meter | graduations | fader, all three sharing the remaining height
	meterBounds = inner.removeFromLeft (kMeterWidth);
	inner.removeFromLeft (kColumnGap);
	scaleBounds = inner.removeFromLeft (kScaleWidth);
	inner.removeFromLeft (kColumnGap);
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
	drawScale (g);
}

void ChannelStrip::drawMeter (juce::Graphics& g) const
{
	if (meterBounds.isEmpty())
	{
		return;
	}

	// The gradient is anchored to the full bar, so a partially filled bar already shows the
	// colours of the level it reaches: green at the bottom, amber around -6 dB, red from 0 dB up.
	juce::ColourGradient gradient (theme::meterLow(),
								   (float) meterBounds.getCentreX(), (float) meterBounds.getBottom(),
								   theme::meterHigh(),
								   (float) meterBounds.getCentreX(), (float) meterBounds.getY(), false);
	gradient.addColour (meter::dbToPosition (-18.0f), theme::meterLow());
	gradient.addColour (meter::dbToPosition (-6.0f), theme::meterMid());
	gradient.addColour (meter::dbToPosition (0.0f), theme::meterHigh());

	const auto drawBar = [&g, &gradient] (juce::Rectangle<int> bar, float level)
	{
		g.setColour (theme::track());
		g.fillRoundedRectangle (bar.toFloat(), 1.5f);

		// the height follows the dB scale, so quiet material keeps a usable resolution
		const float filled = (float) bar.getHeight() * meter::levelToPosition (level);
		if (filled >= 1.0f)
		{
			g.setGradientFill (gradient);
			g.fillRoundedRectangle (bar.toFloat().withTop ((float) bar.getBottom() - filled), 1.5f);
		}
	};

	auto bars = meterBounds;
	const int barWidth = juce::jmax (2, (bars.getWidth() - kMeterGap) / 2);
	drawBar (bars.removeFromLeft (barWidth), peakL);
	bars.removeFromLeft (kMeterGap);
	drawBar (bars.removeFromLeft (barWidth), peakR);
}

void ChannelStrip::drawScale (juce::Graphics& g) const
{
	if (scaleBounds.isEmpty())
	{
		return;
	}

	for (int i = 0; i < meter::tickCount; ++i)
	{
		const float position = meter::dbToPosition (meter::tickDbs[i]);
		const float y = (float) scaleBounds.getBottom() - position * (float) scaleBounds.getHeight();
		const bool isZeroDb = (meter::tickDbs[i] == 0.0f);

		// 0 dB is the one graduation that matters most - it gets a darker tick and label
		g.setColour (isZeroDb ? theme::primaryText().withAlpha (0.6f)
							  : theme::secondaryText().withAlpha (0.45f));
		g.fillRect ((float) scaleBounds.getX(), y - 0.5f, 5.0f, 1.0f);

		g.setColour (isZeroDb ? theme::primaryText() : theme::secondaryText());
		g.setFont (juce::Font (juce::FontOptions (7.5f, isZeroDb ? juce::Font::bold : juce::Font::plain)));
		g.drawText (juce::String (meter::tickLabels[i]),
					scaleBounds.withTrimmedLeft (6).withHeight (9).withY (juce::roundToInt (y) - 4),
					juce::Justification::centred, false);
	}
}

} // namespace vgmplay::ui
