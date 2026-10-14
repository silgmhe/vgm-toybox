#include "TransportPanel.h"

#include "Icons.h"
#include "Theme.h"

#include "vgm/VgmEngine.h"

namespace vgmplay::ui
{
namespace
{
	constexpr int kButtonSize = 34;
	constexpr int kLoopMarkerHeight = 12;

	int loopBoxIdForCount (int loops)
	{
		return (loops == 1) ? 1 : (loops == 2) ? 2 : (loops == 3) ? 3 : 4;
	}

	int fadeBoxIdForSeconds (double seconds)
	{
		return (seconds < 0.5) ? 1 : (seconds < 2.0) ? 2 : (seconds < 4.0) ? 3 : (seconds < 8.0) ? 4 : 5;
	}

	juce::String loopTooltip (int loops)
	{
		return (loops == 0) ? "Looping forever - click to play once"
							: "Play once - click to loop forever";
	}
}

TransportPanel::TransportPanel (vgmplay::VgmEngine& engineToUse)
	: engine (engineToUse)
{
	// --- transport buttons ---
	playButton.onClick = [this] { engine.play(); refresh(); };
	pauseButton.onClick = [this] { engine.pause(); refresh(); };
	stopButton.onClick = [this] { engine.stop(); refresh(); };
	loopButton.onClick = [this] { setLoopForever (! loopButton.getToggleState()); };
	playButton.setTooltip ("Play (space)");
	pauseButton.setTooltip ("Pause (space)");
	stopButton.setTooltip ("Stop (reset to the beginning)");
	for (auto* button : { &playButton, &pauseButton, &stopButton, &loopButton })
	{
		button->setColour (juce::TextButton::buttonColourId, theme::card());
		button->setColour (juce::TextButton::buttonOnColourId, theme::accent());
		button->setColour (juce::TextButton::textColourOffId, theme::primaryText());
		button->setColour (juce::TextButton::textColourOnId, juce::Colours::white);
		addAndMakeVisible (*button);
	}

	// --- seek bar ---
	positionSlider.setSliderStyle (juce::Slider::LinearHorizontal);
	positionSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
	positionSlider.setRange (0.0, 1.0, 0.0);
	positionSlider.setEnabled (false);
	positionSlider.setTooltip ("Playback position");
	positionSlider.onDragStart = [this] { userIsDragging = true; };
	positionSlider.onDragEnd = [this]
	{
		userIsDragging = false;
		engine.setPositionSeconds (positionSlider.getValue() * engine.getDurationSeconds());
	};
	addAndMakeVisible (positionSlider);

	// --- volume ---
	volumeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
	// These colours have to be set on the slider itself and *before* the text box is created:
	// the LookAndFeel copies them into the text box label when it creates it, and at that moment
	// this slider has no parent yet, so the lookup would fall back to the default (dark)
	// LookAndFeel - the number would stay white for the lifetime of that label.
	volumeSlider.setColour (juce::Slider::textBoxTextColourId, theme::primaryText());
	volumeSlider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
	volumeSlider.setColour (juce::Slider::textBoxOutlineColourId, theme::border());
	volumeSlider.setColour (juce::Slider::textBoxHighlightColourId, theme::accent().withAlpha (0.25f));
	// the text formatting has to be in place before the text box is created as well, otherwise
	// the box keeps showing the default formatting of the initial value
	volumeSlider.setRange (0.0, 2.0, 0.01);
	volumeSlider.setTextValueSuffix (" %");
	volumeSlider.textFromValueFunction = [] (double value) { return juce::String (juce::roundToInt (value * 100.0)); };
	volumeSlider.valueFromTextFunction = [] (const juce::String& text) { return text.getDoubleValue() / 100.0; };
	volumeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 22);
	volumeSlider.setValue (engine.getMasterVolume(), juce::dontSendNotification);
	volumeSlider.updateText();
	volumeSlider.setTooltip ("Master volume");
	volumeSlider.onValueChange = [this] { engine.setMasterVolume ((float) volumeSlider.getValue()); };
	addAndMakeVisible (volumeSlider);

	// --- loop / fade settings ---
	loopBox.addItemList ({ "1x", "2x", "3x", "Loop forever" }, 1);
	loopBox.setTooltip ("How often the song loops before it stops");
	loopBox.onChange = [this]
	{
		static const int loopCounts[] = { 1, 2, 3, 0 };
		const int index = juce::jlimit (0, 3, loopBox.getSelectedId() - 1);
		engine.setLoopCount (loopCounts[index]);
		refresh();
	};
	addAndMakeVisible (loopBox);

	fadeBox.addItemList ({ "No fade", "1 s fade", "3 s fade", "5 s fade", "10 s fade" }, 1);
	fadeBox.setTooltip ("Fade out time at the end of the last loop");
	fadeBox.onChange = [this]
	{
		static const double fadeTimes[] = { 0.0, 1.0, 3.0, 5.0, 10.0 };
		const int index = juce::jlimit (0, 4, fadeBox.getSelectedId() - 1);
		engine.setFadeSeconds (fadeTimes[index]);
	};
	addAndMakeVisible (fadeBox);

	fadeBox.setSelectedId (fadeBoxIdForSeconds (engine.getFadeSeconds()), juce::dontSendNotification);

	refresh();
}

void TransportPanel::resized (void)
{
	auto inner = getLocalBounds().reduced (theme::cardPadding);

	auto row = inner.removeFromTop (kButtonSize);
	playButton.setBounds (row.removeFromLeft (kButtonSize));
	row.removeFromLeft (6);
	pauseButton.setBounds (row.removeFromLeft (kButtonSize));
	row.removeFromLeft (6);
	stopButton.setBounds (row.removeFromLeft (kButtonSize));
	row.removeFromLeft (18);
	loopButton.setBounds (row.removeFromLeft (kButtonSize));

	inner.removeFromTop (5);
	loopMarkerBounds = inner.removeFromTop (kLoopMarkerHeight);
	inner.removeFromTop (1);
	positionSlider.setBounds (inner.removeFromTop (22));
	inner.removeFromTop (theme::rowGap);

	auto settingsRow = inner.removeFromTop (26);
	loopBox.setBounds (settingsRow.removeFromLeft (108));
	settingsRow.removeFromLeft (8);
	fadeBox.setBounds (settingsRow.removeFromLeft (118));
	settingsRow.removeFromLeft (8);
	volumeSlider.setBounds (settingsRow);
}

void TransportPanel::paintCardContent (juce::Graphics& g)
{
	// loop point marker above the position slider (only for songs that have a loop point)
	if (loopMarkerNormalised < 0.0 || loopMarkerBounds.isEmpty())
	{
		return;
	}

	const float iconSize = (float) kLoopMarkerHeight - 1.0f;
	const float travel = (float) loopMarkerBounds.getWidth() - 2.0f * theme::sliderThumbRadius;
	const float centreX = (float) loopMarkerBounds.getX() + theme::sliderThumbRadius
							+ juce::jlimit (0.0f, 1.0f, (float) loopMarkerNormalised) * travel;
	const auto iconArea = juce::Rectangle<float> (iconSize, iconSize)
							  .withCentre ({ centreX, (float) loopMarkerBounds.getCentreY() });

	// a hairline down to the track turns it into a cue marker; the slider thumb covers it
	// while the position is at the loop point
	const float lineBottom = (float) positionSlider.getBounds().getCentreY();
	if (lineBottom > iconArea.getBottom())
	{
		g.setColour (theme::accent().withAlpha (0.4f));
		g.fillRect (centreX - 0.5f, iconArea.getBottom(), 1.0f, lineBottom - iconArea.getBottom());
	}

	drawRepeatGlyph (g, iconArea, theme::accent());
}

void TransportPanel::refresh (void)
{
	const bool loaded = engine.isLoaded();
	const bool playing = engine.isPlaying();
	const double duration = engine.getDurationSeconds();

	playButton.setToggleState (loaded && playing, juce::dontSendNotification);
	pauseButton.setToggleState (loaded && ! playing && ! engine.isFinished(), juce::dontSendNotification);
	positionSlider.setEnabled (loaded);

	// The loop button and the loop combo box show the same engine setting. Compare them against
	// the controls themselves instead of a cached value and repaint them explicitly: both
	// setToggleState() and setSelectedId() skip their repaint when they think that nothing
	// changed, which left the button looking "on" while the combo box said "1x".
	const int loops = engine.getLoopCount();
	const bool loopForever = (loops == 0);
	const int loopId = loopBoxIdForCount (loops);
	if (loops != lastLoopCount)
	{
		lastLoopCount = loops;
		loopButton.setTooltip (loopTooltip (loops));
	}
	if (loopButton.getToggleState() != loopForever)
	{
		loopButton.setToggleState (loopForever, juce::dontSendNotification);
		loopButton.repaint();
	}
	if (loopBox.getSelectedId() != loopId)
	{
		loopBox.setSelectedId (loopId, juce::dontSendNotification);
		loopBox.repaint();
	}

	// fade box: the setting can also change from the outside (the host restores a session)
	const int fadeId = fadeBoxIdForSeconds (engine.getFadeSeconds());
	if (fadeBox.getSelectedId() != fadeId)
	{
		fadeBox.setSelectedId (fadeId, juce::dontSendNotification);
		fadeBox.repaint();
	}

	// loop point marker (negative = nothing to show; the value only changes when another file
	// is loaded or the sample rate changed)
	const double loopPoint = loaded ? engine.getLoopPointSeconds() : -1.0;
	const double normalised = (loopPoint >= 0.0 && duration > 0.0)
								  ? juce::jlimit (0.0, 1.0, loopPoint / duration) : -1.0;
	if (normalised != loopMarkerNormalised)
	{
		loopMarkerNormalised = normalised;
		repaint (loopMarkerBounds);
	}

	// play position (the slider is left alone while the user drags it)
	if (! userIsDragging)
	{
		const double position = engine.getPositionSeconds();
		const double sliderValue = (duration > 0.0) ? juce::jlimit (0.0, 1.0, position / duration) : 0.0;
		positionSlider.setValue (sliderValue, juce::dontSendNotification);
	}
}

void TransportPanel::togglePlayPause (void)
{
	if (engine.isPlaying())
	{
		engine.pause();
	}
	else
	{
		engine.play();
	}
	refresh();
}

void TransportPanel::seekRelative (double seconds)
{
	engine.setPositionSeconds (engine.getPositionSeconds() + seconds);
}

void TransportPanel::setLoopForever (bool shouldLoopForever)
{
	engine.setLoopCount (shouldLoopForever ? 0 : 1);
	refresh();
}

} // namespace vgmplay::ui
