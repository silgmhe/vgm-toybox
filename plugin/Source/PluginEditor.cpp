#include "PluginEditor.h"

using namespace vgmplay;

namespace
{
	constexpr int kMargin = 14;
	constexpr int kCardGap = 10;
	constexpr int kCardPadding = 12;
	constexpr int kTopRowHeight = 32;
	constexpr int kTransportHeight = 132;		// 34 (buttons) + 5 + 12 (loop marker) + 1 + 22 (slider) + 8 + 26 + padding
	constexpr int kRowGap = 8;
	constexpr int kTransportButtonSize = 34;
	constexpr int kLoopMarkerHeight = 12;
	constexpr float kCardRadius = 10.0f;
	constexpr float kButtonRadius = 8.0f;
	constexpr float kSliderThumbRadius = 6.5f;	// must match LightLookAndFeel::drawLinearSlider

	// --- macOS-flavoured light palette -------------------------------------------------
	juce::Colour windowColour (void)		{ return juce::Colour (0xfff2f2f7); }
	juce::Colour cardColour (void)			{ return juce::Colour (0xffffffff); }
	juce::Colour accentColour (void)		{ return juce::Colour (0xff0a84ff); }
	juce::Colour primaryTextColour (void)	{ return juce::Colour (0xff1d1d1f); }
	juce::Colour secondaryTextColour (void)	{ return juce::Colour (0xff74747a); }
	juce::Colour borderColour (void)		{ return juce::Colour (0xffd7d7dd); }
	juce::Colour trackColour (void)			{ return juce::Colour (0xffdcdce2); }
	juce::Colour errorColour (void)			{ return juce::Colour (0xffd70015); }

	/** Draws one of the white "cards" the editor is built from. */
	void drawCard (juce::Graphics& g, juce::Rectangle<int> bounds)
	{
		if (bounds.isEmpty())
		{
			return;
		}

		const auto area = bounds.toFloat();
		g.setColour (juce::Colours::black.withAlpha (0.05f));
		g.fillRoundedRectangle (area.translated (0.0f, 1.0f), kCardRadius);
		g.setColour (cardColour());
		g.fillRoundedRectangle (area, kCardRadius);
		g.setColour (borderColour());
		g.drawRoundedRectangle (area.reduced (0.5f), kCardRadius, 1.0f);
	}

	/** The "repeat" glyph: a ring with an arrow head on the top and the bottom edge. */
	void drawRepeatGlyph (juce::Graphics& g, juce::Rectangle<float> iconArea, juce::Colour colour)
	{
		g.setColour (colour);

		juce::Path ring;
		ring.addRoundedRectangle (iconArea.getX(), iconArea.getY(), iconArea.getWidth(), iconArea.getHeight(),
								  iconArea.getWidth() * 0.34f);
		g.strokePath (ring, juce::PathStrokeType (juce::jmax (1.1f, iconArea.getWidth() * 0.12f),
												  juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

		const float head = iconArea.getWidth() * 0.36f;
		juce::Path heads;
		heads.addTriangle (iconArea.getRight() - head, iconArea.getY() - head * 0.5f,
						   iconArea.getRight() - head, iconArea.getY() + head * 0.5f,
						   iconArea.getRight(), iconArea.getY());
		heads.addTriangle (iconArea.getX() + head, iconArea.getBottom() - head * 0.5f,
						   iconArea.getX() + head, iconArea.getBottom() + head * 0.5f,
						   iconArea.getX(), iconArea.getBottom());
		g.fillPath (heads);
	}

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

	juce::String formatTime (double seconds)
	{
		if (seconds < 0.0 || ! std::isfinite (seconds))
		{
			seconds = 0.0;
		}
		const int totalCentiSeconds = (int) std::lround (seconds * 100.0);
		const int minutes = totalCentiSeconds / 6000;
		const int secs = (totalCentiSeconds / 100) % 60;
		const int cents = totalCentiSeconds % 100;

		return juce::String::formatted ("%d:%02d.%02d", minutes, secs, cents);
	}
}

//==============================================================================
// LightLookAndFeel

VGMPlayAudioProcessorEditor::LightLookAndFeel::LightLookAndFeel (void)
{
	setColourScheme (juce::LookAndFeel_V4::getLightColourScheme());

	setColour (juce::ResizableWindow::backgroundColourId,		windowColour());
	setColour (juce::Label::textColourId,						primaryTextColour());

	setColour (juce::TextButton::buttonColourId,				cardColour());
	setColour (juce::TextButton::buttonOnColourId,				accentColour());
	setColour (juce::TextButton::textColourOffId,				primaryTextColour());
	setColour (juce::TextButton::textColourOnId,				juce::Colours::white);

	setColour (juce::ComboBox::backgroundColourId,				cardColour());
	setColour (juce::ComboBox::textColourId,					primaryTextColour());
	setColour (juce::ComboBox::outlineColourId,					juce::Colours::transparentBlack);
	setColour (juce::ComboBox::arrowColourId,					secondaryTextColour());

	setColour (juce::PopupMenu::backgroundColourId,				cardColour());
	setColour (juce::PopupMenu::textColourId,					primaryTextColour());
	setColour (juce::PopupMenu::highlightedBackgroundColourId,	accentColour());
	setColour (juce::PopupMenu::highlightedTextColourId,		juce::Colours::white);

	setColour (juce::Slider::backgroundColourId,				trackColour());
	setColour (juce::Slider::trackColourId,						accentColour());
	setColour (juce::Slider::thumbColourId,						juce::Colours::white);
	setColour (juce::Slider::textBoxTextColourId,				primaryTextColour());
	setColour (juce::Slider::textBoxBackgroundColourId,			juce::Colours::transparentBlack);
	setColour (juce::Slider::textBoxOutlineColourId,			juce::Colours::transparentBlack);
	setColour (juce::Slider::textBoxHighlightColourId,			accentColour().withAlpha (0.25f));

	setColour (juce::ToggleButton::textColourId,				primaryTextColour());
	setColour (juce::ToggleButton::tickColourId,				accentColour());
	setColour (juce::ToggleButton::tickDisabledColourId,		borderColour());

	setColour (juce::TooltipWindow::backgroundColourId,			juce::Colour (0xf7ffffff));
	setColour (juce::TooltipWindow::textColourId,				primaryTextColour());
	setColour (juce::TooltipWindow::outlineColourId,			borderColour());
}

void VGMPlayAudioProcessorEditor::LightLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
	const juce::Colour& backgroundColour, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
	const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
	auto fill = backgroundColour.withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f);

	if (shouldDrawButtonAsDown)
	{
		fill = fill.darker (0.10f);
	}
	else if (shouldDrawButtonAsHighlighted)
	{
		fill = fill.darker (0.05f);
	}

	g.setColour (fill);
	g.fillRoundedRectangle (bounds, kButtonRadius);

	// a hairline keeps light buttons readable; a filled accent button does not need one
	if (fill.getPerceivedBrightness() > 0.6f && ! button.getToggleState())
	{
		g.setColour (borderColour());
		g.drawRoundedRectangle (bounds.reduced (0.5f), kButtonRadius, 1.0f);
	}
}

void VGMPlayAudioProcessorEditor::LightLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button,
	bool /*shouldDrawButtonAsHighlighted*/, bool /*shouldDrawButtonAsDown*/)
{
	g.setFont (juce::Font (juce::FontOptions (13.5f, juce::Font::bold)));
	g.setColour (button.findColour (button.getToggleState() ? juce::TextButton::textColourOnId
															: juce::TextButton::textColourOffId)
					 .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));
	g.drawFittedText (button.getButtonText(), button.getLocalBounds().reduced (6, 0),
					  juce::Justification::centred, 1, 0.9f);
}

void VGMPlayAudioProcessorEditor::LightLookAndFeel::drawTickBox (juce::Graphics& g, juce::Component& component,
	float x, float y, float w, float h, bool ticked, bool isEnabled,
	bool shouldDrawButtonAsHighlighted, bool /*shouldDrawButtonAsDown*/)
{
	const auto box = juce::Rectangle<float> (x, y, w, h).reduced (w * 0.1f);
	constexpr float radius = 4.0f;

	if (ticked)
	{
		g.setColour (isEnabled ? component.findColour (juce::ToggleButton::tickColourId)
							   : component.findColour (juce::ToggleButton::tickDisabledColourId));
		g.fillRoundedRectangle (box, radius);

		juce::Path tick;
		const float size = box.getWidth();
		tick.startNewSubPath (box.getX() + size * 0.24f, box.getY() + size * 0.52f);
		tick.lineTo (box.getX() + size * 0.43f, box.getY() + size * 0.71f);
		tick.lineTo (box.getX() + size * 0.78f, box.getY() + size * 0.29f);
		g.setColour (juce::Colours::white);
		g.strokePath (tick, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
	}
	else
	{
		g.setColour (juce::Colours::white);
		g.fillRoundedRectangle (box, radius);
		g.setColour (shouldDrawButtonAsHighlighted ? secondaryTextColour().withAlpha (0.6f) : juce::Colour (0xffc5c5cb));
		g.drawRoundedRectangle (box.reduced (0.5f), radius, 1.0f);
	}
}

void VGMPlayAudioProcessorEditor::LightLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
	bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
	const auto area = button.getLocalBounds().toFloat();
	const float boxSize = juce::jmin (15.0f, area.getHeight() - 4.0f);
	const auto box = juce::Rectangle<float> (boxSize, boxSize).withCentre ({ area.getX() + 9.0f, area.getCentreY() });

	drawTickBox (g, button, box.getX(), box.getY(), box.getWidth(), box.getHeight(),
				 button.getToggleState(), button.isEnabled(), shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);

	g.setColour (button.findColour (juce::ToggleButton::textColourId)
				   .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));
	g.setFont (juce::Font (juce::FontOptions (12.5f)));
	g.drawFittedText (button.getButtonText(), area.withTrimmedLeft (box.getRight() + 6.0f).toNearestInt(),
					  juce::Justification::centredLeft, 1, 0.85f);
}

void VGMPlayAudioProcessorEditor::LightLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
	float sliderPos, float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style, juce::Slider& slider)
{
	if (style != juce::Slider::LinearHorizontal)
	{
		juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
		return;
	}

	const bool enabled = slider.isEnabled();
	const float centreY = (float) y + (float) height * 0.5f;
	constexpr float trackHeight = 4.0f;
	constexpr float thumbRadius = kSliderThumbRadius;	// shared with the loop point marker

	const juce::Rectangle<float> track ((float) x, centreY - trackHeight * 0.5f, (float) width, trackHeight);
	g.setColour (slider.findColour (juce::Slider::backgroundColourId).withMultipliedAlpha (enabled ? 1.0f : 0.6f));
	g.fillRoundedRectangle (track, trackHeight * 0.5f);

	// getSliderLayout() has already inset the slider bounds by the thumb radius, so sliderPos
	// maps directly onto the track (insetting it again would shift the thumb against the mouse)
	const float thumbX = juce::jlimit (track.getX(), track.getRight(), sliderPos);

	if (enabled && thumbX > track.getX() + 1.0f)
	{
		g.setColour (slider.findColour (juce::Slider::trackColourId));
		g.fillRoundedRectangle (track.withRight (thumbX), trackHeight * 0.5f);
	}

	const auto thumb = juce::Rectangle<float> (thumbRadius * 2.0f, thumbRadius * 2.0f).withCentre ({ thumbX, centreY });
	g.setColour (juce::Colours::black.withAlpha (enabled ? 0.12f : 0.06f));
	g.fillEllipse (thumb.translated (0.0f, 0.5f));
	g.setColour (slider.findColour (juce::Slider::thumbColourId));
	g.fillEllipse (thumb);
	g.setColour (juce::Colour (0xffc5c5cb));
	g.drawEllipse (thumb, 1.0f);
}

void VGMPlayAudioProcessorEditor::LightLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height,
	bool /*isButtonDown*/, int /*buttonX*/, int /*buttonY*/, int /*buttonW*/, int /*buttonH*/, juce::ComboBox& box)
{
	const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);

	g.setColour (cardColour().withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.5f));
	g.fillRoundedRectangle (bounds, 6.0f);
	g.setColour (borderColour());
	g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);

	juce::Path chevron;
	const float centreX = (float) width - 14.0f;
	const float centreY = (float) height * 0.5f;
	chevron.startNewSubPath (centreX - 4.0f, centreY - 2.0f);
	chevron.lineTo (centreX, centreY + 2.5f);
	chevron.lineTo (centreX + 4.0f, centreY - 2.0f);
	g.setColour (secondaryTextColour());
	g.strokePath (chevron, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font VGMPlayAudioProcessorEditor::LightLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
	return juce::Font (juce::FontOptions (12.5f));
}

void VGMPlayAudioProcessorEditor::LightLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
	label.setBounds (10, 1, box.getWidth() - 28, box.getHeight() - 2);
	label.setFont (getComboBoxFont (box));
	label.setJustificationType (juce::Justification::centredLeft);
}

//==============================================================================
// IconButton

void VGMPlayAudioProcessorEditor::IconButton::paintButton (juce::Graphics& g, bool isMouseOverButton, bool isButtonDown)
{
	const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
	const bool toggled = getToggleState();

	auto background = toggled ? findColour (juce::TextButton::buttonOnColourId)
							 : findColour (juce::TextButton::buttonColourId);
	auto iconColour = toggled ? findColour (juce::TextButton::textColourOnId)
							  : findColour (juce::TextButton::textColourOffId);

	if (! isEnabled())
	{
		background = background.withMultipliedAlpha (0.5f);
		iconColour = iconColour.withMultipliedAlpha (0.4f);
	}
	else if (isButtonDown)
	{
		background = background.darker (0.10f);
	}
	else if (isMouseOverButton)
	{
		background = background.darker (0.05f);
	}

	g.setColour (background);
	g.fillRoundedRectangle (bounds, kButtonRadius);

	if (! toggled)
	{
		g.setColour (borderColour());
		g.drawRoundedRectangle (bounds.reduced (0.5f), kButtonRadius, 1.0f);
	}

	const auto iconArea = bounds.reduced (bounds.getWidth() * 0.28f, bounds.getHeight() * 0.28f);
	g.setColour (iconColour);

	if (icon == Icon::play)
	{
		juce::Path path;
		path.addTriangle (iconArea.getX(), iconArea.getY(),
						  iconArea.getX(), iconArea.getBottom(),
						  iconArea.getRight(), iconArea.getCentreY());
		g.fillPath (path);
	}
	else if (icon == Icon::pause)
	{
		juce::Path path;
		const float barWidth = iconArea.getWidth() * 0.32f;
		path.addRectangle (iconArea.getX(), iconArea.getY(), barWidth, iconArea.getHeight());
		path.addRectangle (iconArea.getRight() - barWidth, iconArea.getY(), barWidth, iconArea.getHeight());
		g.fillPath (path);
	}
	else if (icon == Icon::stop)
	{
		g.fillRoundedRectangle (iconArea, 1.5f);
	}
	else
	{
		drawRepeatGlyph (g, iconArea, iconColour);
	}
}

//==============================================================================
// editor

VGMPlayAudioProcessorEditor::VGMPlayAudioProcessorEditor (VGMPlayAudioProcessor& p)
	: AudioProcessorEditor (&p), processor (p), engine (p.getEngine())
{
	setLookAndFeel (&lookAndFeel);

	// --- open button (primary action) ---
	openButton.onClick = [this] { openFileChooser(); };
	openButton.setColour (juce::TextButton::buttonColourId, accentColour());
	openButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
	openButton.setTooltip ("Open a .vgm / .vgz file");
	addAndMakeVisible (openButton);

	// --- transport ---
	playButton.onClick = [this] { engine.play(); updateControls(); };
	pauseButton.onClick = [this] { engine.pause(); updateControls(); };
	stopButton.onClick = [this] { engine.stop(); updateControls(); };
	loopButton.onClick = [this] { setLoopForever (! loopButton.getToggleState()); };
	playButton.setTooltip ("Play (space)");
	pauseButton.setTooltip ("Pause (space)");
	stopButton.setTooltip ("Stop (reset to the beginning)");
	for (auto* button : { &playButton, &pauseButton, &stopButton, &loopButton })
	{
		button->setColour (juce::TextButton::buttonColourId, cardColour());
		button->setColour (juce::TextButton::buttonOnColourId, accentColour());
		button->setColour (juce::TextButton::textColourOffId, primaryTextColour());
		button->setColour (juce::TextButton::textColourOnId, juce::Colours::white);
		addAndMakeVisible (*button);
	}

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

	volumeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
	// These colours have to be set on the slider itself and *before* the text box is created:
	// the LookAndFeel copies them into the text box label when it creates it, and at that moment
	// this slider has no parent yet, so the lookup would fall back to the default (dark)
	// LookAndFeel - the number would stay white for the lifetime of that label.
	volumeSlider.setColour (juce::Slider::textBoxTextColourId, primaryTextColour());
	volumeSlider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
	volumeSlider.setColour (juce::Slider::textBoxOutlineColourId, borderColour());
	volumeSlider.setColour (juce::Slider::textBoxHighlightColourId, accentColour().withAlpha (0.25f));
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

	loopBox.addItemList ({ "1x", "2x", "3x", "Loop forever" }, 1);
	loopBox.setTooltip ("How often the song loops before it stops");
	loopBox.onChange = [this]
	{
		static const int loopCounts[] = { 1, 2, 3, 0 };
		const int index = juce::jlimit (0, 3, loopBox.getSelectedId() - 1);
		engine.setLoopCount (loopCounts[index]);
		updateControls();
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

	// --- labels ---
	titleLabel.setFont (juce::Font (juce::FontOptions (18.0f, juce::Font::bold)));
	titleLabel.setColour (juce::Label::textColourId, primaryTextColour());
	titleLabel.setJustificationType (juce::Justification::centredLeft);
	titleLabel.setText ("No file loaded", juce::dontSendNotification);
	addAndMakeVisible (titleLabel);

	detailsLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
	detailsLabel.setColour (juce::Label::textColourId, secondaryTextColour());
	detailsLabel.setJustificationType (juce::Justification::centredLeft);
	addAndMakeVisible (detailsLabel);

	infoLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
	infoLabel.setColour (juce::Label::textColourId, secondaryTextColour());
	infoLabel.setJustificationType (juce::Justification::centredLeft);
	infoLabel.setText ("Load a .vgm / .vgz file with the button above or drop one onto this window.",
					   juce::dontSendNotification);
	addAndMakeVisible (infoLabel);

	statusLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
	statusLabel.setColour (juce::Label::textColourId, secondaryTextColour());
	statusLabel.setJustificationType (juce::Justification::centredRight);
	addAndMakeVisible (statusLabel);

	dropHintLabel.setFont (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)));
	dropHintLabel.setColour (juce::Label::textColourId, accentColour());
	dropHintLabel.setJustificationType (juce::Justification::centred);
	dropHintLabel.setText ("Drop a VGM / VGZ file here", juce::dontSendNotification);
	dropHintLabel.setVisible (false);
	// addChildComponent() keeps it hidden - addAndMakeVisible() would make the full-size
	// label visible and swallow every mouse click in the editor
	addChildComponent (dropHintLabel);

	// --- state ---
	fadeBox.setSelectedId (fadeBoxIdForSeconds (engine.getFadeSeconds()), juce::dontSendNotification);

	updateControls();
	rebuildChipButtons();

	setResizable (true, true);
	setResizeLimits (640, 420, 1400, 900);
	setSize (780, 470);
	startTimerHz (20);
}

VGMPlayAudioProcessorEditor::~VGMPlayAudioProcessorEditor()
{
	stopTimer();
	setLookAndFeel (nullptr);
}

//==============================================================================
// painting / layout

void VGMPlayAudioProcessorEditor::paint (juce::Graphics& g)
{
	g.fillAll (windowColour());

	drawCard (g, infoCardBounds);
	drawCard (g, chipsCardBounds);
	drawCard (g, transportCardBounds);

	if (! chipsHeaderBounds.isEmpty())
	{
		g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
		g.setColour (secondaryTextColour());
		g.drawText ("SOUND CHIPS", chipsHeaderBounds, juce::Justification::centredLeft);
		g.drawText ("UNCHECK TO MUTE", chipsHeaderBounds, juce::Justification::centredRight);
	}

	// loop point marker above the position slider (only for songs that have a loop point)
	if (loopMarkerNormalised >= 0.0 && ! loopMarkerBounds.isEmpty())
	{
		const float iconSize = (float) kLoopMarkerHeight - 1.0f;
		const float travel = (float) loopMarkerBounds.getWidth() - 2.0f * kSliderThumbRadius;
		const float centreX = (float) loopMarkerBounds.getX() + kSliderThumbRadius
								+ juce::jlimit (0.0f, 1.0f, (float) loopMarkerNormalised) * travel;
		const auto iconArea = juce::Rectangle<float> (iconSize, iconSize)
								  .withCentre ({ centreX, (float) loopMarkerBounds.getCentreY() });

		// a hairline down to the track turns it into a cue marker; the slider thumb covers it
		// while the position is at the loop point
		const float lineBottom = (float) positionSlider.getBounds().getCentreY();
		if (lineBottom > iconArea.getBottom())
		{
			g.setColour (accentColour().withAlpha (0.4f));
			g.fillRect (centreX - 0.5f, iconArea.getBottom(), 1.0f, lineBottom - iconArea.getBottom());
		}

		drawRepeatGlyph (g, iconArea, accentColour());
	}

	// dashed outline while a file is dragged over the editor
	if (dropHintLabel.isVisible())
	{
		juce::Path outline, dashed;
		outline.addRoundedRectangle (getLocalBounds().toFloat().reduced (5.0f), 12.0f);
		const float dashes[] = { 8.0f, 6.0f };
		juce::PathStrokeType (2.0f).createDashedStroke (dashed, outline, dashes, 2);
		g.setColour (accentColour().withAlpha (0.75f));
		g.fillPath (dashed);
	}
}

void VGMPlayAudioProcessorEditor::resized (void)
{
	auto area = getLocalBounds().reduced (kMargin);

	// top row: open button + status
	auto topRow = area.removeFromTop (kTopRowHeight);
	openButton.setBounds (topRow.removeFromLeft (140));
	topRow.removeFromLeft (kCardGap);
	statusLabel.setBounds (topRow);
	area.removeFromTop (kCardGap);

	// transport card (always at the bottom)
	const int transportHeight = juce::jmin (kTransportHeight, juce::jmax (90, area.getHeight() - 110));
	transportCardBounds = area.removeFromBottom (transportHeight);
	{
		auto inner = transportCardBounds.reduced (kCardPadding);
		auto row = inner.removeFromTop (kTransportButtonSize);
		playButton.setBounds (row.removeFromLeft (kTransportButtonSize));
		row.removeFromLeft (6);
		pauseButton.setBounds (row.removeFromLeft (kTransportButtonSize));
		row.removeFromLeft (6);
		stopButton.setBounds (row.removeFromLeft (kTransportButtonSize));
		row.removeFromLeft (18);
		loopButton.setBounds (row.removeFromLeft (kTransportButtonSize));

		inner.removeFromTop (5);
		loopMarkerBounds = inner.removeFromTop (kLoopMarkerHeight);
		inner.removeFromTop (1);
		positionSlider.setBounds (inner.removeFromTop (22));
		inner.removeFromTop (kRowGap);

		auto settingsRow = inner.removeFromTop (26);
		loopBox.setBounds (settingsRow.removeFromLeft (108));
		settingsRow.removeFromLeft (8);
		fadeBox.setBounds (settingsRow.removeFromLeft (118));
		settingsRow.removeFromLeft (8);
		volumeSlider.setBounds (settingsRow);
	}
	area.removeFromBottom (kCardGap);

	// info card
	const int infoHeight = juce::jmin (96, juce::jmax (70, area.getHeight() - 80));
	infoCardBounds = area.removeFromTop (infoHeight);
	{
		auto inner = infoCardBounds.reduced (kCardPadding, 10);
		titleLabel.setBounds (inner.removeFromTop (26));
		detailsLabel.setBounds (inner.removeFromTop (20));
		infoLabel.setBounds (inner.removeFromTop (18));
	}
	area.removeFromTop (kCardGap);

	// chip mute buttons (+ section header) - the card disappears when no file is loaded
	chipsCardBounds = {};
	chipsHeaderBounds = {};

	if (! chipButtons.isEmpty())
	{
		const int perRow = juce::jmax (1, (area.getWidth() - 2 * kCardPadding) / 118);
		const int rows = ((int) chipButtons.size() + perRow - 1) / perRow;
		const int height = juce::jmin (22 + rows * 30 + 6, area.getHeight());

		if (height > 0)
		{
			chipsCardBounds = area.removeFromTop (height);
			auto inner = chipsCardBounds.reduced (kCardPadding, 8);
			chipsHeaderBounds = inner.removeFromTop (20);

			juce::FlexBox flex;
			flex.flexWrap = juce::FlexBox::Wrap::wrap;
			flex.justifyContent = juce::FlexBox::JustifyContent::flexStart;
			flex.alignContent = juce::FlexBox::AlignContent::flexStart;
			for (auto* button : chipButtons)
			{
				flex.items.add (juce::FlexItem (*button).withMinWidth (112.0f).withHeight (26.0f)
									.withMargin ({ 0.0f, 6.0f, 4.0f, 0.0f }));
			}
			flex.performLayout (inner);
		}
	}

	dropHintLabel.setBounds (getLocalBounds().reduced (kMargin));

	// The cards, the section header and the loop marker are painted from the bounds computed
	// above, and changing those bounds does not repaint anything by itself (adding or removing
	// child components only repaints the children). Without this, a file loaded through the
	// file chooser showed its chip checkboxes but neither the chips card nor its header.
	repaint();
}

//==============================================================================
// interaction

bool VGMPlayAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
	if (key == juce::KeyPress::spaceKey)
	{
		if (engine.isPlaying())
		{
			engine.pause();
		}
		else
		{
			engine.play();
		}
		updateControls();
		return true;
	}
	if (key == juce::KeyPress::homeKey)
	{
		engine.setPositionSeconds (0.0);
		return true;
	}
	if (key == juce::KeyPress::leftKey)
	{
		seekRelative (-5.0);
		return true;
	}
	if (key == juce::KeyPress::rightKey)
	{
		seekRelative (5.0);
		return true;
	}
	return false;
}

void VGMPlayAudioProcessorEditor::seekRelative (double seconds)
{
	engine.setPositionSeconds (engine.getPositionSeconds() + seconds);
}

void VGMPlayAudioProcessorEditor::setLoopForever (bool shouldLoopForever)
{
	engine.setLoopCount (shouldLoopForever ? 0 : 1);
	updateControls();
}

bool VGMPlayAudioProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
	for (const auto& path : files)
	{
		if (path.endsWithIgnoreCase (".vgm") || path.endsWithIgnoreCase (".vgz"))
		{
			return true;
		}
	}
	return false;
}

void VGMPlayAudioProcessorEditor::fileDragEnter (const juce::StringArray&, int, int)
{
	dropHintLabel.setVisible (true);
	dropHintLabel.toFront (false);
	repaint();
}

void VGMPlayAudioProcessorEditor::fileDragExit (const juce::StringArray&)
{
	dropHintLabel.setVisible (false);
	repaint();
}

void VGMPlayAudioProcessorEditor::filesDropped (const juce::StringArray& files, int, int)
{
	dropHintLabel.setVisible (false);
	repaint();		// erase the dashed drop outline (like fileDragExit() does)

	for (const auto& path : files)
	{
		if (path.endsWithIgnoreCase (".vgm") || path.endsWithIgnoreCase (".vgz"))
		{
			loadFile (juce::File (path));
			break;
		}
	}
}

void VGMPlayAudioProcessorEditor::openFileChooser (void)
{
	const juce::File startDir = engine.getFile().existsAsFile() ? engine.getFile().getParentDirectory()
																: juce::File::getSpecialLocation (juce::File::userMusicDirectory);
	fileChooser = std::make_unique<juce::FileChooser> ("Open a VGM file", startDir, "*.vgm;*.vgz");
	fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
							  [this] (const juce::FileChooser& chooser)
							  {
								  const juce::File result = chooser.getResult();
								  if (result.existsAsFile())
								  {
									  loadFile (result);
								  }
							  });
}

void VGMPlayAudioProcessorEditor::loadFile (const juce::File& file)
{
	juce::String errorMessage;
	if (! engine.loadFile (file, errorMessage))
	{
		lastError = errorMessage;
	}
	else
	{
		lastError.clear();
		positionSlider.setValue (0.0, juce::dontSendNotification);
	}

	rebuildChipButtons();
	updateControls();
}

//==============================================================================
// periodic refresh

void VGMPlayAudioProcessorEditor::timerCallback (void)
{
	updateControls();

	if (! userIsDragging)
	{
		const double duration = engine.getDurationSeconds();
		const double position = engine.getPositionSeconds();
		const double normalised = (duration > 0.0) ? juce::jlimit (0.0, 1.0, position / duration) : 0.0;
		positionSlider.setValue (normalised, juce::dontSendNotification);
	}
}

void VGMPlayAudioProcessorEditor::updateControls (void)
{
	const bool loaded = engine.isLoaded();
	const bool playing = engine.isPlaying();
	const double duration = engine.getDurationSeconds();
	const double position = engine.getPositionSeconds();

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
	const bool loopChanged = (loops != lastLoopCount);

	if (loopChanged)
	{
		lastLoopCount = loops;
		loopButton.setTooltip (loopTooltip (loops));
	}
	if (loopChanged || loopButton.getToggleState() != loopForever)
	{
		loopButton.setToggleState (loopForever, juce::dontSendNotification);
		loopButton.repaint();
	}
	if (loopChanged || loopBox.getSelectedId() != loopId)
	{
		loopBox.setSelectedId (loopId, juce::dontSendNotification);
		loopBox.repaint();
	}

	// loop point marker above the position slider (negative = nothing to show; the value only
	// changes when another file is loaded or the sample rate changed)
	{
		const double loopPoint = loaded ? engine.getLoopPointSeconds() : -1.0;
		const double normalised = (loopPoint >= 0.0 && duration > 0.0)
									  ? juce::jlimit (0.0, 1.0, loopPoint / duration) : -1.0;
		if (normalised != loopMarkerNormalised)
		{
			loopMarkerNormalised = normalised;
			repaint (loopMarkerBounds);
		}
	}

	// song info
	if (loaded)
	{
		const auto& file = engine.getFile();
		juce::String title = engine.getTag ("TITLE");
		if (title.isEmpty())
		{
			title = file.getFileNameWithoutExtension();
		}
		titleLabel.setText (title, juce::dontSendNotification);

		juce::String details;
		const auto appendTag = [&details] (const juce::String& name, const juce::String& value)
		{
			if (value.isNotEmpty())
			{
				details << name << ": " << value << "    ";
			}
		};
		appendTag ("Game", engine.getTag ("GAME"));
		appendTag ("System", engine.getTag ("SYSTEM"));
		appendTag ("Artist", engine.getTag ("ARTIST"));
		appendTag ("Date", engine.getTag ("DATE"));
		detailsLabel.setText (details.trim(), juce::dontSendNotification);

		infoLabel.setText (file.getFileName() + "   -   " + formatTime (position) + " / " + formatTime (duration),
						   juce::dontSendNotification);
	}
	else
	{
		titleLabel.setText ("No file loaded", juce::dontSendNotification);
		detailsLabel.setText ({}, juce::dontSendNotification);
		infoLabel.setText ("Load a .vgm / .vgz file with the button above or drop one onto this window.",
						   juce::dontSendNotification);
	}

	// status line
	juce::String status;
	if (lastError.isNotEmpty())
	{
		status = lastError;
	}
	else if (! loaded)
	{
		status = "No file";
	}
	else if (engine.isFinished())
	{
		status = "Finished";
	}
	else if (playing)
	{
		status = "Playing";
	}
	else
	{
		status = "Paused";
	}

	if (loaded)
	{
		status << "   " << formatTime (position) << " / " << formatTime (duration);
	}

	statusLabel.setColour (juce::Label::textColourId,
						   lastError.isNotEmpty() ? errorColour() : secondaryTextColour());
	statusLabel.setText (status, juce::dontSendNotification);
}

void VGMPlayAudioProcessorEditor::rebuildChipButtons (void)
{
	chipButtons.clear();
	const auto& chipList = engine.getChips();

	for (size_t i = 0; i < chipList.size(); ++i)
	{
		auto* button = new juce::ToggleButton (chipList[i].name);
		button->setToggleState (! chipList[i].muted, juce::dontSendNotification);	// ticked = audible
		button->setColour (juce::ToggleButton::textColourId, primaryTextColour());
		button->setTooltip (chipList[i].core.isNotEmpty()
								? chipList[i].name + " (" + chipList[i].core + ") - uncheck to mute"
								: chipList[i].name + " - uncheck to mute");
		const size_t index = i;
		button->onClick = [this, index, button]
		{
			engine.setChipMuted (index, ! button->getToggleState());
		};
		chipButtons.add (button);
		addAndMakeVisible (button);
	}

	resized();
}
