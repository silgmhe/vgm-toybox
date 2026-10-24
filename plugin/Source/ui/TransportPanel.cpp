#include "TransportPanel.h"

#include "ButtonPaint.h"
#include "Icons.h"
#include "MeterScale.h"
#include "SoftShadow.h"
#include "SyncGroup.h"
#include "Theme.h"

#include "i18n/LanguageManager.h"
#include "playback/DawSync.h"
#include "playback/PlaybackEngine.h"
#include "playback/TransportPresets.h"

#include <cmath>
#include <cstddef>

namespace toybox::ui
{
namespace
{
	// The buttons reserve their own shadow room inside this (ButtonPaint.h), so the size here is
	// the face plus 2 * buttonShadowInset - a 40 px box paints a 36 px face, slightly larger than
	// the 34 px the buttons used to be before they had a shadow.
	constexpr int kButtonSize = 40;
	constexpr int kLoopMarkerHeight = 12;

	// From the top of the button row down to the buttons: the same 9 the sync group's frame uses,
	// so the transport buttons and the group's controls share one baseline (see SyncGroup).
	constexpr int kRowTopPadding = 9;

	// Between the button row and the loop marker band.
	constexpr int kGapBelowRow = 3;

	juce::String loopTooltip (int loops)
	{
		return (loops == 0) ? i18n::tr ("transport.loopForeverTooltip")
							: i18n::tr ("transport.playOnceTooltip");
	}

}

/**
	The number entry the trim knob opens, drawn over the card instead of in a window of its own.

	Same reasoning as AboutOverlay: a plugin editor that opens a desktop window can leave a 0x0
	window behind while its modal state blocks every click, so everything modal in this project
	is an overlay. The card is 142 px tall, which holds three rows comfortably - the title, the
	editor and the two buttons.
*/
class TransportPanel::ValueEntryOverlay : public juce::Component
{
public:
	ValueEntryOverlay (void)
	{
		setVisible (false);

		title.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
		title.setJustificationType (juce::Justification::centredLeft);
		title.setInterceptsMouseClicks (false, false);
		addAndMakeVisible (title);

		editor.setJustification (juce::Justification::centredLeft);
		editor.setFont (juce::Font (juce::FontOptions (14.0f)));
		editor.setSelectAllWhenFocused (true);
		editor.setInputRestrictions (12, "-.0123456789");
		editor.onReturnKey = [this] { accept(); };
		editor.onEscapeKey = [this] { cancel(); };
		addAndMakeVisible (editor);

		okButton.onClick = [this] { accept(); };
		cancelButton.onClick = [this] { cancel(); };
		addAndMakeVisible (okButton);
		addAndMakeVisible (cancelButton);

		lookAndFeelChanged();
		refreshLanguage();
	}

	void refreshLanguage (void)
	{
		okButton.setButtonText (i18n::tr ("common.ok"));
		cancelButton.setButtonText (i18n::tr ("common.cancel"));
	}

	/** The typed number. Called once, on OK or Return. */
	std::function<void (double)> onAccept;

	/** Re-applies the colours the entry sets on its own controls - setColour() stores a copy,
		so a theme change does not reach them by itself. */
	void lookAndFeelChanged (void) override
	{
		title.setColour (juce::Label::textColourId, theme::primaryText());
		editor.setColour (juce::TextEditor::backgroundColourId, theme::card());
		editor.setColour (juce::TextEditor::textColourId, theme::primaryText());
		editor.setColour (juce::TextEditor::outlineColourId, theme::border());
		editor.setColour (juce::TextEditor::focusedOutlineColourId, theme::accent());
		editor.setColour (juce::TextEditor::highlightColourId, theme::accent().withAlpha (0.20f));
		editor.setColour (juce::CaretComponent::caretColourId, theme::accent());
		okButton.setColour (juce::TextButton::buttonColourId, theme::accent());
		okButton.setColour (juce::TextButton::textColourOffId, theme::onAccent());
	}

	void open (const juce::String& titleText, double initialValue, std::function<void (double)> acceptCallback)
	{
		title.setText (titleText, juce::dontSendNotification);
		editor.setText (juce::String (initialValue, 2), false);
		onAccept = std::move (acceptCallback);

		setVisible (true);
		toFront (true);
		editor.grabKeyboardFocus();
		editor.selectAll();
	}

	void paint (juce::Graphics& g) override
	{
		// The scrim is rounded like the card it covers: the overlay's own bounds are rectangles,
		// so a square fill would paint over the card's rounded corners.
		g.setColour (juce::Colours::black.withAlpha (0.30f));
		g.fillRoundedRectangle (getLocalBounds().toFloat(), theme::cardRadius);

		const auto box = boxBounds().toFloat();
		g.setColour (theme::card());
		g.fillRoundedRectangle (box, theme::buttonRadius);
		g.setColour (theme::cardBorder());
		g.drawRoundedRectangle (box.reduced (0.5f), theme::buttonRadius, 1.0f);
	}

	void resized (void) override
	{
		auto inner = boxBounds().reduced (14, 12);
		title.setBounds (inner.removeFromTop (16));
		inner.removeFromTop (6);
		editor.setBounds (inner.removeFromTop (26));
		inner.removeFromTop (8);

		auto row = inner.removeFromTop (26);
		cancelButton.setBounds (row.removeFromRight (74));
		row.removeFromRight (8);
		okButton.setBounds (row.removeFromRight (74));
	}

	void mouseDown (const juce::MouseEvent& event) override
	{
		// a click on the scrim, outside the entry box, is a cancel - like the About box
		if (! boxBounds().contains (event.getPosition()))
		{
			cancel();
		}
	}

	bool keyPressed (const juce::KeyPress& key) override
	{
		if (key == juce::KeyPress::escapeKey)
		{
			cancel();
			return true;
		}

		return false;
	}

private:
	juce::Rectangle<int> boxBounds (void) const
	{
		const int width = juce::jmin (330, juce::jmax (240, getWidth() - 40));
		const int height = juce::jmin (108, juce::jmax (100, getHeight() - 16));
		return getLocalBounds().withSizeKeepingCentre (width, height);
	}

	void accept (void)
	{
		const auto text = editor.getText().trim();

		if (text.isEmpty())
		{
			cancel();
			return;
		}

		// the callback is taken before hiding: it may open the entry again, and a stale member
		// would be cleared on the way
		auto accepted = onAccept;
		close();

		if (accepted != nullptr)
		{
			accepted (text.getDoubleValue());
		}
	}

	void cancel (void)
	{
		close();
	}

	void close (void)
	{
		onAccept = nullptr;
		setVisible (false);
	}

	juce::Label title;
	juce::TextEditor editor;
	juce::TextButton okButton;
	juce::TextButton cancelButton;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ValueEntryOverlay)
};

TransportPanel::TransportPanel (toybox::PlaybackEngine& engineToUse, bool syncAvailableToUse)
	: engine (engineToUse),
	  sync (engineToUse.getDawSync())
{
	// --- transport buttons ---
	playButton.onClick = [this] { startPlayback(); };
	pauseButton.onClick = [this] { pausePlayback(); };
	stopButton.onClick = [this] { stopTransport(); };
	loopButton.onClick = [this] { setLoopForever (! loopButton.getToggleState()); };
	cursorButton.onClick = [this] { setStartCursorEnabled (! cursorButton.getToggleState()); };

	// The buttons take the LookAndFeel's own colours (card face, accent while on) - there is
	// nothing to set on them, so a palette change reaches them by itself.
	for (auto* button : { &playButton, &pauseButton, &stopButton, &loopButton, &cursorButton })
	{
		addAndMakeVisible (*button);
	}
	cursorButton.setToggleState (startCursorEnabled, juce::dontSendNotification);

	// --- DAW sync (ui/SyncGroup.h, state in playback/DawSync.h) ---
	// A standalone has no play head, so there is nothing to follow: the group is not created at
	// all there (docs/desgin/daw-sync-cue-design.md, §7.4). Either way the sync flag is cleared
	// for a standalone - its transport lock has no button to unlock it, so a session saved with
	// sync on must not come back locked.
	if (syncAvailableToUse)
	{
		syncGroup = std::make_unique<SyncGroup> (engineToUse);

		// The card's transport is gated by the group's switch, and the number entry the group's
		// menu opens covers the whole card - so both are the card's to answer.
		syncGroup->onStateChanged = [this] { refresh(); };
		syncGroup->onRequestValueEntry = [this] (const juce::String& entryTitle, double initialValue,
												 std::function<void (double)> onAccept)
		{
			openValueEntry (entryTitle, initialValue, std::move (onAccept));
		};

		addAndMakeVisible (*syncGroup);
	}
	else
	{
		sync.setEnabled (false);
	}

	// The number entry is a child of the card, created once and only shown on demand.
	entryOverlay = std::make_unique<ValueEntryOverlay>();
	addChildComponent (*entryOverlay);

	// --- playback start cursor (the drag strip above the seek bar) ---
	startMarkerDragArea.setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
	addAndMakeVisible (startMarkerDragArea);

	// --- seek bar ---
	positionSlider.setSliderStyle (juce::Slider::LinearHorizontal);
	positionSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
	positionSlider.setRange (0.0, 1.0, 0.0);
	positionSlider.setComponentID ("slider.position");	// for the window scripts (AppHarnessTest)
	positionSlider.setEnabled (false);
	positionSlider.onDragStart = [this] { userIsDragging = true; };
	positionSlider.onDragEnd = [this]
	{
		userIsDragging = false;
		seekToSeconds (positionSlider.getValue() * engine.getDurationSeconds());
	};
	addAndMakeVisible (positionSlider);

	// --- volume ---
	volumeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
	// The slider's text box colours are applied by lookAndFeelChanged(), and they have to be in
	// place *before* the box is created below - which is why it is called here rather than at the
	// end of the constructor.
	lookAndFeelChanged();
	// the text formatting has to be in place before the text box is created as well, otherwise
	// the box keeps showing the default formatting of the initial value
	// the travel follows the same dB scale as the mixer meters (the value stays a linear gain)
	volumeSlider.setNormalisableRange (meter::gainRange());
	volumeSlider.setTextValueSuffix (" %");
	volumeSlider.textFromValueFunction = [] (double value) { return juce::String (juce::roundToInt (value * 100.0)); };
	volumeSlider.valueFromTextFunction = [] (const juce::String& text) { return text.getDoubleValue() / 100.0; };
	volumeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 22);
	volumeSlider.setValue (engine.getMasterVolume(), juce::dontSendNotification);
	volumeSlider.updateText();
	volumeSlider.onValueChange = [this] { engine.setMasterVolume ((float) volumeSlider.getValue()); };
	addAndMakeVisible (volumeSlider);

	// --- loop / fade settings ---
	loopBox.onChange = [this]
	{
		const int index = juce::jlimit (0, (int) transport::loopPresets.size() - 1,
									   loopBox.getSelectedId() - 1);
		engine.setLoopCount (transport::loopPresets[(std::size_t) index].count);
		refresh();
	};
	addAndMakeVisible (loopBox);

	fadeBox.onChange = [this]
	{
		const int index = juce::jlimit (0, (int) transport::fadePresets.size() - 1,
									   fadeBox.getSelectedId() - 1);
		engine.setFadeSeconds (transport::fadePresets[(std::size_t) index].seconds);
	};
	addAndMakeVisible (fadeBox);

	fadeBox.setSelectedId (transport::fadePresetIndex (engine.getFadeSeconds()) + 1,
						   juce::dontSendNotification);

	refreshLanguage();
}

void TransportPanel::refreshLanguage (void)
{
	playButton.setButtonText (i18n::tr ("transport.play"));
	pauseButton.setButtonText (i18n::tr ("transport.pause"));
	stopButton.setButtonText (i18n::tr ("transport.stop"));
	loopButton.setButtonText (i18n::tr ("transport.loop"));
	cursorButton.setButtonText (i18n::tr ("transport.playbackStart"));

	playButton.setTooltip (i18n::tr ("transport.playTooltip"));
	pauseButton.setTooltip (i18n::tr ("transport.pauseTooltip"));
	stopButton.setTooltip (i18n::tr ("transport.stopTooltip"));
	cursorButton.setTooltip (i18n::tr ("transport.cursorTooltip"));
	startMarkerDragArea.setTooltip (i18n::tr ("transport.startMarkerTooltip"));
	positionSlider.setTooltip (i18n::tr ("transport.positionTooltip"));
	volumeSlider.setTooltip (i18n::tr ("transport.volumeTooltip"));
	loopBox.setTooltip (i18n::tr ("transport.loopTooltip"));
	fadeBox.setTooltip (i18n::tr ("transport.fadeTooltip"));

	const int selectedLoop = loopBox.getSelectedId();
	loopBox.clear (juce::dontSendNotification);

	for (std::size_t i = 0; i < transport::loopPresets.size(); ++i)
	{
		loopBox.addItem (i18n::tr (transport::loopPresets[i].labelKey), (int) i + 1);
	}

	if (selectedLoop > 0)
	{
		loopBox.setSelectedId (selectedLoop, juce::dontSendNotification);
	}

	const int selectedFade = fadeBox.getSelectedId();
	fadeBox.clear (juce::dontSendNotification);

	for (std::size_t i = 0; i < transport::fadePresets.size(); ++i)
	{
		fadeBox.addItem (i18n::tr (transport::fadePresets[i].labelKey), (int) i + 1);
	}

	if (selectedFade > 0)
	{
		fadeBox.setSelectedId (selectedFade, juce::dontSendNotification);
	}

	if (syncGroup != nullptr)
	{
		syncGroup->refreshLanguage();
	}

	if (entryOverlay != nullptr)
	{
		entryOverlay->refreshLanguage();
	}

	refresh();
}

// The destructor lives here because the two things it owns are only defined in this file (the
// header only knows that they exist - a std::unique_ptr to an incomplete type needs the
// definition wherever the pointer is destroyed).
TransportPanel::~TransportPanel (void) = default;

void TransportPanel::lookAndFeelChanged (void)
{
	// The slider that shows a number in a text box (the volume's percentage): its colours have to
	// be set on the slider itself, because that is where the box's label copies them from when it
	// is created - and the box is recreated whenever the LookAndFeel changes
	// (Slider::colourChanged() forwards to Slider::lookAndFeelChanged()), so re-applying them here
	// is what keeps the number readable after a theme switch. (The sync group's knob does the same
	// for its own box - see SyncGroup::lookAndFeelChanged.)
	juce::Slider* const numberFields[] = { &volumeSlider };

	for (auto* slider : numberFields)
	{
		slider->setColour (juce::Slider::textBoxTextColourId, theme::primaryText());
		// Must be opaque: JUCE's createSliderTextBox() copies this colour into the *editor* that
		// appears while typing and forces alpha to 1, so a transparent black would turn into an
		// opaque black box with dark text in it.
		slider->setColour (juce::Slider::textBoxBackgroundColourId, theme::card());
		slider->setColour (juce::Slider::textBoxOutlineColourId, theme::border());
		slider->setColour (juce::Slider::textBoxHighlightColourId, theme::accent().withAlpha (0.25f));
	}
}

void TransportPanel::resized (void)
{
	auto inner = getLocalBounds().reduced (theme::cardPadding);

	// The DAW-sync group is one cluster at the right end of the *top row* - it is one component, so
	// the row only has to leave its width free; the transport buttons keep their fixed sizes on the
	// left no matter how narrow the card gets. It is taken out of that row rather than out of the
	// panel: the seek bar and the settings row below own the full width of the card, and a group
	// that stole their width would leave both of them stopping in mid-air.
	//
	// The row is the height of the group's frame even in the standalone, where there is no group -
	// otherwise the transport would sit a few pixels apart in the two builds.
	auto topRow = inner.removeFromTop (SyncGroup::frameHeight());

	if (syncGroup != nullptr)
	{
		auto groupArea = topRow.removeFromRight (juce::jmin (SyncGroup::preferredWidth(), topRow.getWidth()));

		// The nameplate straddles the frame's top edge, so the component is taller than the frame
		// and starts above the row.
		syncGroup->setBounds (groupArea.withTop (groupArea.getY() - SyncGroup::nameplateOverhang()));
	}

	// The transport buttons line up with the controls *inside* the frame, not with its top edge:
	// one baseline across the card is what makes the group read as part of the panel instead of as
	// a box bolted onto a row that happens to be next to it.
	auto row = topRow.withTrimmedTop (kRowTopPadding).withHeight (kButtonSize);
	playButton.setBounds (row.removeFromLeft (kButtonSize));
	row.removeFromLeft (6);
	pauseButton.setBounds (row.removeFromLeft (kButtonSize));
	row.removeFromLeft (6);
	stopButton.setBounds (row.removeFromLeft (kButtonSize));
	row.removeFromLeft (18);
	loopButton.setBounds (row.removeFromLeft (kButtonSize));
	row.removeFromLeft (6);
	cursorButton.setBounds (row.removeFromLeft (kButtonSize));

	// the gap between the button row and the loop marker band
	inner.removeFromTop (kGapBelowRow);
	markerBandBounds = inner.removeFromTop (kLoopMarkerHeight);
	startMarkerDragArea.setBounds (markerBandBounds.withTop (markerBandBounds.getY() - 3));
	inner.removeFromTop (1);
	positionSlider.setBounds (inner.removeFromTop (22));
	inner.removeFromTop (theme::rowGap);

	// 30 = the 26 px face the combo boxes had before they got a shadow + the room they reserve
	auto settingsRow = inner.removeFromTop (30);
	loopBox.setBounds (settingsRow.removeFromLeft (108));
	settingsRow.removeFromLeft (8);
	fadeBox.setBounds (settingsRow.removeFromLeft (118));
	settingsRow.removeFromLeft (8);
	volumeSlider.setBounds (settingsRow);

	// the number entry covers the whole card while it is up
	if (entryOverlay != nullptr)
	{
		entryOverlay->setBounds (getLocalBounds());
	}
}

void TransportPanel::paintCardContent (juce::Graphics& g)
{
	// playback start cursor (the strip above the seek bar drags it - see StartMarkerDragArea)
	paintPlaybackStartCursor (g);

	// The loop and cursor toggles glow in the accent colour while they are on. The card paints them
	// - and therefore behind the buttons: the glow reaches further than the shadow room a button
	// keeps inside its own bounds (ButtonPaint.h). The sync group's switch paints its own glow,
	// clipped to its frame - that is SyncGroup's business, not the card's.
	for (auto* button : { &loopButton, &cursorButton })
	{
		if (button->getToggleState())
		{
			drawButtonGlow (g, button->getBounds());
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
	if (isHostDriving())
	{
		return;
	}

	const double duration = engine.getDurationSeconds();

	// A backend whose length is not known yet reports zero, and clamping to that turns every seek
	// into a seek to the beginning - which looks exactly like a player that ignores the bar. The
	// clamp only applies when there is a length to clamp to.
	if (duration > 0.0)
	{
		seconds = juce::jlimit (0.0, duration, seconds);
	}

	{
		// Temporary: one line per seek, so a report of "it starts over" can be checked against the
		// numbers instead of reproduced by ear.
		const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
							  .getChildFile ("vgmtoybox-seek.log");
		file.appendText (juce::String ("seek ") + juce::String (seconds, 2)
						 + " of " + juce::String (duration, 2) + "\n");
	}

	// Asynchronous: an NSF seek replays the song for every track, and blocking the message thread
	// for that long is what made the bar feel unresponsive on files with an expansion chip.
	engine.requestPositionSeconds (seconds);

	// while the transport is running the cursor stays where it is; a stationary transport
	// carries it along, so the position and the cursor cannot drift apart
	if (startCursorEnabled && ! engine.isPlaying())
	{
		setPlaybackStart (seconds);
	}
}

void TransportPanel::startPlayback (void)
{
	// a finished song starts over at the playback start cursor (PlaybackEngine::play() would
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
	if (isHostDriving())
	{
		return;
	}

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
	engine.pause();
	refresh();
}

void TransportPanel::refresh (void)
{
	const bool loaded = engine.isLoaded();
	const bool playing = engine.isPlaying();
	const bool paused = engine.isPaused();
	const double duration = engine.getDurationSeconds();

	// the button lights follow the transport: playing lights Play, the last stop-like action
	// lights Pause or Stop ("stop" also covers a song that finished). The state lives in the
	// engine so a pause from the tray menu lights Pause here as well.
	playButton.setToggleState (loaded && playing, juce::dontSendNotification);
	pauseButton.setToggleState (loaded && ! playing && paused, juce::dontSendNotification);
	stopButton.setToggleState (loaded && ! playing && ! paused, juce::dontSendNotification);
	positionSlider.setEnabled (loaded && ! isHostDriving());

	// The loop button and the loop combo box show the same engine setting. Compare them against
	// the controls themselves instead of a cached value and repaint them explicitly: both
	// setToggleState() and setSelectedId() skip their repaint when they think that nothing
	// changed, which left the button looking "on" while the combo box said "1x".
	const int loops = engine.getLoopCount();
	const bool loopForever = (loops == 0);
	const int loopId = transport::loopPresetIndex (loops) + 1;

	if (syncGroup != nullptr)
	{
		syncGroup->refresh();
	}

	// While the host drives the transport, a local play/pause/stop/seek would be overwritten by
	// the next block - a button that does nothing reads as broken, so it is disabled instead.
	// The loop count, the fade and the volume stay live: they are part of the mapping.
	const bool hostDrives = isHostDriving();

	if (playButton.isEnabled() == hostDrives)
	{
		playButton.setEnabled (! hostDrives);
		pauseButton.setEnabled (! hostDrives);
		stopButton.setEnabled (! hostDrives);
		cursorButton.setEnabled (! hostDrives);
		startMarkerDragArea.setEnabled (! hostDrives);
	}

	if (loops != lastLoopCount)
	{
		lastLoopCount = loops;
		loopButton.setTooltip (loopTooltip (loops));
	}
	if (loopButton.getToggleState() != loopForever)
	{
		loopButton.setToggleState (loopForever, juce::dontSendNotification);
		// repaint a ring around the button as well - the accent glow (see paintCardContent())
		// reaches past the button bounds
		repaint (buttonGlowBounds (loopButton.getBounds()));
	}
	if (loopBox.getSelectedId() != loopId)
	{
		loopBox.setSelectedId (loopId, juce::dontSendNotification);
		loopBox.repaint();
	}

	// fade box: the setting can also change from the outside (the host restores a session)
	const int fadeId = transport::fadePresetIndex (engine.getFadeSeconds()) + 1;
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

		// A value change repaints the whole seek bar, so movements that would not even shift the
		// thumb by half a pixel are dropped - on a long song that is a window-area present for
		// nothing.
		const double width = (double) juce::jmax (1, positionSlider.getWidth());
		if (std::abs (sliderValue - positionSlider.getValue()) * width >= 0.5)
		{
			positionSlider.setValue (sliderValue, juce::dontSendNotification);
		}
	}
}

bool TransportPanel::isHostDriving (void) const
{
	// The one predicate for "the host owns the transport": the buttons, the seek bar, the key
	// handlers and the space bar all ask this, so those answers cannot drift apart.
	// No group means the standalone, which has no host to give a key back to - even if a project
	// was saved with sync on (the group is not created there, see the constructor).
	return syncGroup != nullptr && syncGroup->isHostDriving();
}

void TransportPanel::togglePlayStop (void)
{
	// the host owns the transport while sync is on - the buttons are disabled too, but the
	// keyboard shortcut does not go through them
	if (isHostDriving())
	{
		return;
	}

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
	if (isHostDriving())
	{
		return;
	}

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

void TransportPanel::openValueEntry (const juce::String& title, double initialValue,
									 std::function<void (double)> onAccept)
{
	if (entryOverlay == nullptr)
	{
		return;
	}

	// the overlay may have been laid out before the card had its size
	entryOverlay->setBounds (getLocalBounds());
	entryOverlay->open (title, initialValue, std::move (onAccept));
}

} // namespace toybox::ui
