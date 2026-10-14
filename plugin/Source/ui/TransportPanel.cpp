#include "TransportPanel.h"

#include "Icons.h"
#include "SoftShadow.h"
#include "Theme.h"

#include "vgm/MixerEngine.h"

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

TransportPanel::TransportPanel (vgmplay::MixerEngine& engineToUse)
	: engine (engineToUse)
{
	// --- transport buttons ---
	playButton.onClick = [this] { startPlayback(); };
	pauseButton.onClick = [this] { pausePlayback(); };
	stopButton.onClick = [this] { stopTransport(); };
	loopButton.onClick = [this] { setLoopForever (! loopButton.getToggleState()); };
	cursorButton.onClick = [this] { setStartCursorEnabled (! cursorButton.getToggleState()); };
	playButton.setTooltip ("Play (space)");
	pauseButton.setTooltip ("Pause (enter)");
	stopButton.setTooltip ("Stop - return to the playback start marker");
	cursorButton.setTooltip ("Playback start cursor - when off, Stop goes back to the beginning of the song");
	for (auto* button : { &playButton, &pauseButton, &stopButton, &loopButton, &cursorButton })
	{
		button->setColour (juce::TextButton::buttonColourId, theme::card());
		button->setColour (juce::TextButton::buttonOnColourId, theme::accent());
		button->setColour (juce::TextButton::textColourOffId, theme::primaryText());
		button->setColour (juce::TextButton::textColourOnId, juce::Colours::white);
		addAndMakeVisible (*button);
	}
	cursorButton.setToggleState (startCursorEnabled, juce::dontSendNotification);

	// --- playback start cursor (the drag strip above the seek bar) ---
	startMarkerDragArea.setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
	startMarkerDragArea.setTooltip ("Playback start - drag to set where playback begins and Stop returns to");
	addAndMakeVisible (startMarkerDragArea);

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
		seekToSeconds (positionSlider.getValue() * engine.getDurationSeconds());
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
	row.removeFromLeft (6);
	cursorButton.setBounds (row.removeFromLeft (kButtonSize));

	inner.removeFromTop (5);
	markerBandBounds = inner.removeFromTop (kLoopMarkerHeight);
	startMarkerDragArea.setBounds (markerBandBounds.withTop (markerBandBounds.getY() - 3));
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
	// playback start cursor (the strip above the seek bar drags it - see StartMarkerDragArea)
	paintPlaybackStartCursor (g);

	// The loop and the cursor toggle glow in the accent colour while they are on. The card
	// paints them (and therefore behind the buttons): a component cannot draw outside its
	// own bounds.
	for (auto* button : { &loopButton, &cursorButton })
	{
		if (button->getToggleState())
		{
			drawSoftShadow (g, button->getBounds().toFloat(), theme::buttonRadius,
							theme::accentGlow(), theme::accentGlowBlur, { 0, theme::accentGlowOffsetY });
		}
	}

	// loop point marker above the position slider (only for songs that have a loop point)
	if (loopMarkerNormalised < 0.0 || markerBandBounds.isEmpty())
	{
		return;
	}

	const float iconSize = (float) kLoopMarkerHeight - 1.0f;
	const float travel = (float) markerBandBounds.getWidth() - 2.0f * theme::sliderThumbRadius;
	const float centreX = (float) markerBandBounds.getX() + theme::sliderThumbRadius
							+ juce::jlimit (0.0f, 1.0f, (float) loopMarkerNormalised) * travel;
	const auto iconArea = juce::Rectangle<float> (iconSize, iconSize)
							  .withCentre ({ centreX, (float) markerBandBounds.getCentreY() });

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

void TransportPanel::paintPlaybackStartCursor (juce::Graphics& g) const
{
	if (! startCursorEnabled || ! engine.isLoaded() || markerBandBounds.isEmpty())
	{
		return;
	}

	const double duration = engine.getDurationSeconds();
	const float normalised = (duration > 0.0) ? juce::jlimit (0.0f, 1.0f, (float) (playbackStartSeconds / duration)) : 0.0f;
	const float centreX = (float) markerBandBounds.getX() + theme::sliderThumbRadius
							+ normalised * ((float) markerBandBounds.getWidth() - 2.0f * theme::sliderThumbRadius);

	const float iconSize = (float) kLoopMarkerHeight - 2.0f;
	const auto iconArea = juce::Rectangle<float> (iconSize, iconSize)
							  .withCentre ({ centreX, (float) markerBandBounds.getCentreY() });

	// a hairline down to the track marks the position, like the loop point marker's
	const float lineBottom = (float) positionSlider.getBounds().getCentreY();
	if (lineBottom > iconArea.getBottom())
	{
		g.setColour (theme::primaryText().withAlpha (0.30f));
		g.fillRect (centreX - 0.5f, iconArea.getBottom(), 1.0f, lineBottom - iconArea.getBottom());
	}

	drawPlayStartGlyph (g, iconArea, theme::primaryText());
}

void TransportPanel::movePlaybackStartToBandX (int bandX)
{
	const double duration = engine.getDurationSeconds();
	const float travel = (float) markerBandBounds.getWidth() - 2.0f * theme::sliderThumbRadius;
	if (! startCursorEnabled || ! engine.isLoaded() || duration <= 0.0 || travel <= 1.0f)
	{
		return;
	}

	const float normalised = juce::jlimit (0.0f, 1.0f, ((float) bandX - theme::sliderThumbRadius) / travel);

	setPlaybackStart ((double) normalised * duration);

	// a stationary transport follows the cursor, so the next Play (or space) starts right
	// there; while playing the marker only moves - the song keeps rolling
	if (! engine.isPlaying())
	{
		engine.setPositionSeconds (playbackStartSeconds);
		refresh();
	}
}

juce::Rectangle<int> TransportPanel::playbackStartRepaintArea (double seconds) const
{
	const double duration = engine.getDurationSeconds();
	if (! engine.isLoaded() || duration <= 0.0 || markerBandBounds.isEmpty())
	{
		return {};
	}

	const float normalised = juce::jlimit (0.0f, 1.0f, (float) (seconds / duration));
	const float centreX = (float) markerBandBounds.getX() + theme::sliderThumbRadius
							+ normalised * ((float) markerBandBounds.getWidth() - 2.0f * theme::sliderThumbRadius);

	const int top = markerBandBounds.getY() - 2;
	const int bottom = positionSlider.getBounds().getCentreY() + 2;
	return juce::Rectangle<int> (juce::roundToInt (centreX) - kLoopMarkerHeight, top,
								 2 * kLoopMarkerHeight, bottom - top);
}

void TransportPanel::setPlaybackStart (double seconds)
{
	// repaint the columns of the old and the new marker - the hairline reaches down to
	// the slider, and repainting the whole card on every drag event would be wasteful
	const auto oldArea = playbackStartRepaintArea (playbackStartSeconds);
	playbackStartSeconds = seconds;
	repaint (oldArea.getUnion (playbackStartRepaintArea (playbackStartSeconds)));
}

void TransportPanel::seekToSeconds (double seconds)
{
	const double duration = engine.getDurationSeconds();
	seconds = juce::jlimit (0.0, duration, seconds);
	engine.setPositionSeconds (seconds);

	// while the transport is running the cursor stays where it is; a stationary transport
	// carries it along, so the position and the cursor cannot drift apart
	if (startCursorEnabled && ! engine.isPlaying())
	{
		setPlaybackStart (seconds);
	}
}

void TransportPanel::startPlayback (void)
{
	paused = false;

	// a finished song starts over at the playback start cursor (MixerEngine::play() would
	// restart it from the very beginning - which is what happens with the cursor off)
	if (startCursorEnabled && engine.isFinished())
	{
		engine.setPositionSeconds (playbackStartSeconds);
	}

	engine.play();
	refresh();
}

void TransportPanel::stopTransport (void)
{
	paused = false;
	engine.stop();

	// with the cursor switched off the transport behaves like a player: Stop goes back to
	// the beginning of the song
	if (startCursorEnabled)
	{
		engine.setPositionSeconds (playbackStartSeconds);
	}

	refresh();
}

void TransportPanel::setStartCursorEnabled (bool shouldBeEnabled)
{
	startCursorEnabled = shouldBeEnabled;
	cursorButton.setToggleState (shouldBeEnabled, juce::dontSendNotification);
	startMarkerDragArea.setVisible (shouldBeEnabled);
	repaint();	// the marker and the button's glow appear/disappear
}

void TransportPanel::pausePlayback (void)
{
	paused = true;
	engine.pause();
	refresh();
}

void TransportPanel::refresh (void)
{
	const bool loaded = engine.isLoaded();
	const bool playing = engine.isPlaying();
	const double duration = engine.getDurationSeconds();

	// the button lights follow the transport: playing lights Play, the last stop-like action
	// lights Pause or Stop ("stop" also covers a song that finished)
	playButton.setToggleState (loaded && playing, juce::dontSendNotification);
	pauseButton.setToggleState (loaded && ! playing && paused, juce::dontSendNotification);
	stopButton.setToggleState (loaded && ! playing && ! paused, juce::dontSendNotification);
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
		// repaint a ring around the button as well - the accent glow (see
		// paintCardContent()) reaches past the button bounds
		repaint (loopButton.getBounds().expanded (theme::accentGlowBlur + theme::accentGlowOffsetY + 2));
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

	// the playback start cursor belongs to the song - reset it when another file is loaded
	const auto& file = engine.getFile();
	if (file != lastFile)
	{
		lastFile = file;
		playbackStartSeconds = 0.0;
		repaint();
	}

	// loop point marker (negative = nothing to show; the value only changes when another file
	// is loaded or the sample rate changed)
	const double loopPoint = loaded ? engine.getLoopPointSeconds() : -1.0;
	const double normalised = (loopPoint >= 0.0 && duration > 0.0)
								  ? juce::jlimit (0.0, 1.0, loopPoint / duration) : -1.0;
	if (normalised != loopMarkerNormalised)
	{
		loopMarkerNormalised = normalised;
		repaint (markerBandBounds);
	}

	// play position (the slider is left alone while the user drags it)
	if (! userIsDragging)
	{
		const double position = engine.getPositionSeconds();
		const double sliderValue = (duration > 0.0) ? juce::jlimit (0.0, 1.0, position / duration) : 0.0;
		positionSlider.setValue (sliderValue, juce::dontSendNotification);
	}
}

void TransportPanel::togglePlayStop (void)
{
	if (! startCursorEnabled)
	{
		// with the cursor off, space carries the classic media-player binding
		togglePlayPause();
		return;
	}

	if (engine.isPlaying())
	{
		stopTransport();
	}
	else
	{
		startPlayback();
	}
}

void TransportPanel::togglePlayPause (void)
{
	if (engine.isPlaying())
	{
		pausePlayback();
	}
	else
	{
		startPlayback();
	}
}

void TransportPanel::seekRelative (double seconds)
{
	seekToSeconds (engine.getPositionSeconds() + seconds);
}

void TransportPanel::setLoopForever (bool shouldLoopForever)
{
	engine.setLoopCount (shouldLoopForever ? 0 : 1);
	refresh();
}

} // namespace vgmplay::ui
