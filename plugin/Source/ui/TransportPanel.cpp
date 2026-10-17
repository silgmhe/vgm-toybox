#include "TransportPanel.h"

#include "ButtonPaint.h"
#include "Icons.h"
#include "MeterScale.h"
#include "SoftShadow.h"
#include "Theme.h"

#include "playback/DawSync.h"
#include "playback/PlaybackEngine.h"

#include <cmath>

namespace toybox::ui
{
namespace
{
	// The buttons reserve their own shadow room inside this (ButtonPaint.h), so the size here is
	// the face plus 2 * buttonShadowInset - a 40 px box paints a 36 px face, slightly larger than
	// the 34 px the buttons used to be before they had a shadow.
	constexpr int kButtonSize = 40;
	constexpr int kLoopMarkerHeight = 12;

	// The cue trim: a rotary that fits the button row, and the number field beside it (the
	// Slider's own text box - the same box the volume shows its percentage in).
	constexpr int kOffsetKnobSize = 40;		// the rotary area; JUCE lays the text box out beside it
	constexpr int kOffsetValueWidth = 62;
	constexpr int kSyncGroupGap = 14;		// between the Cue button and the knob

	// The DAW-sync group is a dashed frame drawn on the card: the controls are one cluster, and a
	// dashed outline says that without pretending to be a surface. The padding is generous on
	// purpose - a frame the controls touch reads as a selection rectangle drawn around them. The
	// nameplate straddles the top edge, so only its lower half has to clear the controls.
	constexpr int kFramePadding = 8;		// left and right, inside the frame
	constexpr int kFrameTopPadding = 9;		// from the frame's top edge down to the controls
	constexpr int kFrameBottomPadding = 3;
	constexpr int kFrameGapBelow = 3;		// between the frame and the loop marker band

	constexpr int kFrameHeight = kFrameTopPadding + kButtonSize + kFrameBottomPadding;

	// 3 px dashes, 3 off: fine enough to read as a guide rather than as a border
	constexpr float kFrameDash = 3.0f;
	constexpr float kFrameGap = 3.0f;

	// the nameplate: 8 px caps, tracked out, sitting on the frame's top edge
	constexpr float kNameplateTracking = 0.5f;	// extra pixels between the glyphs
	constexpr int kNameplatePadding = 6;		// left and right of the text, inside the gap
	constexpr int kNameplateHeight = 10;		// the gap cut into the dashes behind the text

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

	/** The offset as the knob's number shows it. It is the offset and nothing else: the cue always
		exists, so the field has no "no cue" case to fall back to. */
	juce::String offsetText (double ms)
	{
		const double shown = (std::abs (ms) < 0.05) ? 0.0 : ms;	// never "-0.0"
		return (shown > 0.0 ? "+" : "") + juce::String (shown, 1) + " ms";
	}

	/** The dashed frame around the DAW-sync group: 1 px dashes along a rounded rectangle, and
		nothing else - no fill, no inner shadow, no bevelled edge. A recess has to argue that it is
		structure and not a selection rectangle, and at this size that argument cannot be won; a
		dashed line only ever means "these belong together", so it does not have to make it.

		The dashes come from createDashedStroke() on the whole outline rather than from one
		drawDashedLine() per side: only the stroked version dashes *around* the corners, and a
		frame whose corners are solid reads as a rounded rectangle with dashed sides. */
	void drawGroupFrame (juce::Graphics& g, juce::Rectangle<float> area, float radius,
						 juce::Colour colour)
	{
		if (area.isEmpty())
		{
			return;
		}

		juce::Path outline;
		outline.addRoundedRectangle (area.reduced (0.5f), radius);	// the 1 px stroke lands on the grid

		const float dashes[] = { kFrameDash, kFrameGap };
		juce::Path stroked;
		juce::PathStrokeType (1.0f).createDashedStroke (stroked, outline, dashes, 2);

		g.setColour (colour);
		g.fillPath (stroked);
	}

	/** The nameplate of the DAW-sync group: 8 px bold caps, tracked out letter by letter, sitting
		*on* the frame's top edge with the dashes erased behind it - the legend of a fieldset, and
		how a frame that carries a name is drawn everywhere else.

		The eraser is the card's own colour, not a plate: the dashes are the only thing up there,
		and a raised plate would be a second surface inside a frame that deliberately has none.

		The tracking is applied per glyph (JUCE has no letter-spacing option) and the whole line is
		filled as one path, so it costs one fill and no text layout. */
	void drawGroupNameplate (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> frame,
							 juce::Colour colour)
	{
		if (frame.isEmpty())
		{
			return;
		}

		const juce::Font font { juce::FontOptions (8.0f, juce::Font::bold) };

		juce::GlyphArrangement glyphs;
		glyphs.addLineOfText (font, text, 0.0f, 0.0f);	// baseline at y = 0
		for (int i = 0; i < glyphs.getNumGlyphs(); ++i)
		{
			glyphs.getGlyph (i).moveBy (kNameplateTracking * (float) i, 0.0f);
		}

		juce::Path path;
		glyphs.createPath (path);
		const auto glyphBounds = path.getBounds();

		// the gap in the dashes, centred on the frame
		const int gapWidth = juce::jmin (frame.getWidth() - 2 * kFramePadding,
										 (int) std::ceil (glyphBounds.getWidth()) + 2 * kNameplatePadding);
		const juce::Rectangle<float> gap ((float) frame.getCentreX() - (float) gapWidth * 0.5f,
										  (float) frame.getY() - (float) kNameplateHeight * 0.5f,
										  (float) gapWidth, (float) kNameplateHeight);

		g.setColour (theme::card());
		g.fillRoundedRectangle (gap, 2.0f);

		// Both axes centre the glyph box, not the baseline: "DAW SYNC" is all caps, so the box is
		// the visible ink and centring it is what looks centred.
		const auto shift = gap.getCentre() - glyphBounds.getCentre();

		g.setColour (colour);
		g.fillPath (path, juce::AffineTransform::translation (shift.x, shift.y));
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
	juce::TextButton okButton { "OK" };
	juce::TextButton cancelButton { "Cancel" };

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ValueEntryOverlay)
};

TransportPanel::TransportPanel (toybox::PlaybackEngine& engineToUse, bool syncAvailableToUse)
	: engine (engineToUse),
	  sync (engineToUse.getDawSync()),
	  syncAvailable (syncAvailableToUse)
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
	// The buttons take the LookAndFeel's own colours (card face, accent while on) - there is
	// nothing to set on them, so a palette change reaches them by itself.
	for (auto* button : { &playButton, &pauseButton, &stopButton, &loopButton, &cursorButton })
	{
		addAndMakeVisible (*button);
	}
	cursorButton.setToggleState (startCursorEnabled, juce::dontSendNotification);

	// --- DAW sync (playback/DawSync.h) ---
	// The toggle reads the sync state in refresh() and turns over what it shows, so the button
	// itself stays the one place the state is shown. Both glyphs carry their word: a chain link
	// or a flag alone does not say "this follows your DAW".
	syncButton.setCaption ("SYNC");
	syncButton.setTooltip ("Follow the host's clock - the song position becomes \"host position - cue\". "
						   "Click again to play on its own");
	syncButton.onClick = [this]
	{
		sync.setEnabled (! sync.isEnabled());
		updateSyncControls();
	};

	// Taking the cue is an action, not a toggle and not a state light: the DAW moves, so taking it
	// again is the normal gesture. The button is therefore never lit - a lit face in this editor
	// means "this switch is on", and Cue is not a switch. There is no "has a cue" state to show
	// either: the cue always exists (0 is the DAW's start), and the frame's menu can put it back
	// there.
	cueButton.setCaption ("CUE");
	cueButton.setTooltip ("Take the host's position as the cue point - the place on the DAW timeline "
						  "where the song's beginning plays. Right-click this box to put the cue "
						  "back to 0 s");
	cueButton.onClick = [this] { takeCue(); };

	// The trim knob: a rotary whose number is its own text box - the same edit box the volume
	// slider shows "100 %" in, so an exact offset is typed where it is read. A corner of the row
	// is a small place, and a knob with a hidden value would need a dialog to show it.
	offsetKnob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
	offsetKnob.setTextBoxStyle (juce::Slider::TextBoxRight, false, kOffsetValueWidth, 22);
	offsetKnob.setComponentID ("knob.cue-offset");	// for the window scripts (AppHarnessTest)
	offsetKnob.setRange (-lastKnobRangeMs, lastKnobRangeMs, 0.0);
	offsetKnob.setValue (0.0, juce::dontSendNotification);
	offsetKnob.setTooltip ("Cue offset - drag to trim the cue point by a few milliseconds, or type the "
						   "number beside it. Double-click to put it back to zero, right-click anywhere "
						   "in the box for the knob's range and to reset the cue");
	// The number is the offset, always: the cue is never absent, so this field has no second thing
	// to say.
	offsetKnob.textFromValueFunction = [] (double ms) { return offsetText (ms); };
	offsetKnob.valueFromTextFunction = [] (const juce::String& text) { return text.getDoubleValue(); };
	offsetKnob.onValueChange = [this]
	{
		if (updatingOffsetKnob)
		{
			return;
		}

		sync.setCueOffsetMs (offsetKnob.getValue());
	};

	// A standalone has no play head, so there is nothing to sync to: the two buttons and the trim
	// knob are not created at all there (docs/desgin/daw-sync-cue-design.md, §8).
	if (syncAvailable)
	{
		addAndMakeVisible (syncButton);
		addAndMakeVisible (cueButton);
		addAndMakeVisible (offsetKnob);
	}
	else
	{
		// A standalone has no host to follow, and its transport lock has no button to unlock it:
		// a session saved with sync on must not come back locked.
		sync.setEnabled (false);
	}

	// The frame is the menu's owner, not just the knob: the panel listens to its own children so
	// that a right click on either button or on the knob reaches the same place. A left click is
	// untouched (a Button ignores a right click already, and the knob swallows it), and in the
	// standalone the frame has no bounds, so the listener finds nothing to do.
	addMouseListener (&syncGroupMouseListener, true);

	// The number entry is a child of the card, created once and only shown on demand.
	entryOverlay = std::make_unique<ValueEntryOverlay>();
	addChildComponent (*entryOverlay);

	// --- playback start cursor (the drag strip above the seek bar) ---
	startMarkerDragArea.setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
	startMarkerDragArea.setTooltip ("Playback start - drag to set where playback begins and Stop returns to");
	addAndMakeVisible (startMarkerDragArea);

	// --- seek bar ---
	positionSlider.setSliderStyle (juce::Slider::LinearHorizontal);
	positionSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
	positionSlider.setRange (0.0, 1.0, 0.0);
	positionSlider.setComponentID ("slider.position");	// for the window scripts (AppHarnessTest)
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
	volumeSlider.setTooltip ("Output volume - the last stage of the engine, after the mixer master");
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

// The destructor lives here because the value entry it owns is only defined in this file (the
// header only knows that they exist - a std::unique_ptr to an incomplete type needs the
// definition wherever the pointer is destroyed).
TransportPanel::~TransportPanel (void)
{
	// The listener is a member, so it dies before the base class does: unregister it while both
	// are still alive.
	removeMouseListener (&syncGroupMouseListener);
}

void TransportPanel::lookAndFeelChanged (void)
{
	// The two sliders that show a number in a text box (the volume's percentage and the cue
	// offset): their colours have to be set on the slider itself, because that is where the box's
	// label copies them from when it is created - and the box is recreated whenever the LookAndFeel
	// changes (Slider::colourChanged() forwards to Slider::lookAndFeelChanged()), so re-applying
	// them here is what keeps the number readable after a theme switch.
	juce::Slider* const numberFields[] = { &volumeSlider, &offsetKnob };

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

	// The DAW-sync group is one cluster at the right end of the *top row* - Sync, Cue, then the
	// trim knob and its number - so the transport buttons keep their fixed sizes on the left no
	// matter how narrow the card gets. It is taken out of that row rather than out of the panel:
	// the seek bar and the settings row below own the full width of the card, and a group that
	// stole their width would leave both of them stopping in mid-air.
	auto topRow = inner.removeFromTop (kFrameHeight);
	const int groupWidth = 2 * kFramePadding + kButtonSize + 6 + kButtonSize + kSyncGroupGap
						 + kOffsetKnobSize + kOffsetValueWidth;
	syncGroupBounds = topRow.removeFromRight (juce::jmin (groupWidth, topRow.getWidth()));

	auto syncGroup = syncGroupBounds.withTrimmedLeft (kFramePadding)
									.withTrimmedRight (kFramePadding)
									.withTrimmedTop (kFrameTopPadding)
									.withTrimmedBottom (kFrameBottomPadding);
	syncButton.setBounds (syncGroup.removeFromLeft (kButtonSize));
	syncGroup.removeFromLeft (6);
	cueButton.setBounds (syncGroup.removeFromLeft (kButtonSize));
	syncGroup.removeFromLeft (kSyncGroupGap);
	offsetKnob.setBounds (syncGroup);	// the rotary, and its number field, to the right of it

	// The transport buttons line up with the controls *inside* the frame, not with its top edge:
	// one baseline across the card is what makes the group read as part of the panel instead of as
	// a box bolted onto a row that happens to be next to it.
	auto row = topRow.withTrimmedTop (kFrameTopPadding).withHeight (kButtonSize);
	playButton.setBounds (row.removeFromLeft (kButtonSize));
	row.removeFromLeft (6);
	pauseButton.setBounds (row.removeFromLeft (kButtonSize));
	row.removeFromLeft (6);
	stopButton.setBounds (row.removeFromLeft (kButtonSize));
	row.removeFromLeft (18);
	loopButton.setBounds (row.removeFromLeft (kButtonSize));
	row.removeFromLeft (6);
	cursorButton.setBounds (row.removeFromLeft (kButtonSize));

	// the gap between the frame's bottom edge and the loop marker band
	inner.removeFromTop (kFrameGapBelow);
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

	// The DAW-sync group: a dashed frame with the nameplate on its top edge and the three controls
	// inside (both drawn at the end, over the glow). The frame never changes colour - the version
	// of this that lit the whole outline read as a selection rectangle drawn around the group.
	// Whether the group is live is said by the switch inside it (accent face and glow) and by the
	// nameplate: the thing is lit, not boxed.
	const bool syncFrameVisible = syncAvailable && ! syncGroupBounds.isEmpty();
	const float frameRadius = theme::cardRadius - 2.0f;
	const bool live = syncFrameVisible && sync.isEnabled();

	// The loop and cursor toggles glow in the accent colour while they are on (Cue is not in the
	// list: it is an action, never lit; sync has its own pass below, clipped to its frame). The
	// card paints them - and therefore behind the buttons: the glow reaches further than the
	// shadow room a button keeps inside its own bounds (ButtonPaint.h).
	for (auto* button : { &loopButton, &cursorButton })
	{
		if (button->getToggleState())
		{
			drawButtonGlow (g, button->getBounds());
		}
	}

	// The sync button's glow is clipped to the group's frame: the frame is the box the light is
	// in, and an unclipped halo would wash over the dashes and onto the card around them.
	if (syncButton.getToggleState())
	{
		juce::Graphics::ScopedSaveState save (g);
		if (syncFrameVisible)
		{
			juce::Path frame;
			frame.addRoundedRectangle (syncGroupBounds.toFloat(), frameRadius);
			g.reduceClipRegion (frame);
		}
		drawButtonGlow (g, syncButton.getBounds());
	}

	if (syncFrameVisible)
	{
		// After the glow: the frame closes around the light instead of being tinted by it
		drawGroupFrame (g, syncGroupBounds.toFloat(), frameRadius, theme::groupFrame());

		// the nameplate is the one thing in the group that does carry the state
		drawGroupNameplate (g, "DAW SYNC", syncGroupBounds,
							live ? theme::accent() : theme::secondaryText());
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
	if (sync.isActive())
	{
		return;
	}

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
	if (sync.isActive())
	{
		return;
	}

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
	positionSlider.setEnabled (loaded && ! sync.isActive());

	// The loop button and the loop combo box show the same engine setting. Compare them against
	// the controls themselves instead of a cached value and repaint them explicitly: both
	// setToggleState() and setSelectedId() skip their repaint when they think that nothing
	// changed, which left the button looking "on" while the combo box said "1x".
	const int loops = engine.getLoopCount();
	const bool loopForever = (loops == 0);
	const int loopId = loopBoxIdForCount (loops);
	updateSyncControls();

	// While the host drives the transport, a local play/pause/stop/seek would be overwritten by
	// the next block - a button that does nothing reads as broken, so it is disabled instead.
	// The loop count, the fade and the volume stay live: they are part of the mapping.
	const bool hostDrives = sync.isActive();

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

void TransportPanel::togglePlayStop (void)
{
	// the host owns the transport while sync is on - the buttons are disabled too, but the
	// keyboard shortcut does not go through them
	if (sync.isActive())
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
	if (sync.isActive())
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

void TransportPanel::takeCue (void)
{
	// Always takes: the cue is where the song starts on the DAW timeline, and every click re-anchors
	// it there. It is never cleared - the frame's menu can put it back to 0 s (the DAW's start),
	// which is a cue like any other.
	// setCueFromHost() returns false when the host has no time to give; the button is disabled then,
	// so that is only a race with a host that just stopped reporting.
	sync.setCueFromHost();
	updateSyncControls();
}

void TransportPanel::syncGroupMouseDown (const juce::MouseEvent& event)
{
	if (! syncAvailable || ! event.mods.isPopupMenu())
	{
		return;
	}

	// While the number entry is up it covers the card, and that is its own moment: offering the
	// group's menu over it would be a second way to do the thing already on screen.
	if (entryOverlay != nullptr && entryOverlay->isVisible())
	{
		return;
	}

	// The knob's number field is a TextEditor while it is open, and a right click there is the
	// editor's own (cut / copy / paste). It is a temporary child of the knob, inside the frame.
	auto* const source = event.eventComponent;
	if (source != nullptr
		&& (dynamic_cast<juce::TextEditor*> (source) != nullptr
			|| source->findParentComponentOfClass<juce::TextEditor>() != nullptr))
	{
		return;
	}

	if (! syncGroupBounds.contains (event.getEventRelativeTo (this).getPosition()))
	{
		return;
	}

	// At the click, not at the knob: any point in the frame could have asked for this.
	showOffsetMenu (event.getScreenPosition());
}

void TransportPanel::showOffsetMenu (juce::Point<int> screenPosition)
{
	juce::PopupMenu menu;
	menu.addItem (1, "Exact offset...");
	menu.addItem (2, "Knob range...");
	menu.addSeparator();
	menu.addItem (3, "Reset cue to 0 s");

	// "Exact offset" opens the number field itself (Slider::showTextBox) rather than an overlay:
	// the field is the same edit box the volume uses, so there is one way to type a number, and
	// the menu item is only the discoverable path to it.

	// The menu has to be told this LookAndFeel: PopupMenu only looks at its *own* pointer
	// (menu.lookAndFeel, PopupMenu::setLookAndFeel), never at the target component's, so without
	// this line the menu is drawn by whatever the platform's default LookAndFeel is - the default
	// dark V4 palette over a light card, which is what "the colours do not follow the theme" was.
	// The editor owns the object for the whole time the menu can live.
	menu.setLookAndFeel (&getLookAndFeel());

	// The menu outlives the click that opened it and so does its callback: the panel is tracked
	// rather than captured, because closing the editor destroys it.
	const juce::Component::SafePointer<TransportPanel> safe (this);

	// The target component is not where the menu appears - the area below overrides that - but it
	// is what JUCE scales the menu by (`shouldPopupMenuScaleWithTargetComponent`): without it the
	// menu is drawn at the desktop's scale factor rather than the editor's, which on a HiDPI
	// display makes it noticeably smaller than the window it belongs to.
	menu.showMenuAsync (juce::PopupMenu::Options()
							.withTargetComponent (&offsetKnob)
							.withTargetScreenArea (juce::Rectangle<int> (screenPosition.x,
																		 screenPosition.y, 1, 1)),
						[safe] (int result)
						{
							if (safe == nullptr)
							{
								return;
							}

							if (result == 1)
							{
								safe->offsetKnob.showTextBox();
							}
							else if (result == 2)
							{
								safe->showRangeEntry();
							}
							else if (result == 3)
							{
								// 0 is a cue like any other (the song's beginning on the DAW's
								// beginning), so this is a move, not a clear: nothing downstream
								// has a "no cue" state to fall into.
								safe->sync.setCueSeconds (0.0);
								safe->updateSyncControls();
							}
						});
}

void TransportPanel::showRangeEntry (void)
{
	openValueEntry ("Knob range (+/- ms)", sync.getKnobRangeMs(),
					[this] (double ms)
					{
						// setKnobRangeMs() re-clamps the offset, so the knob can never show a
						// value it cannot reach - updateSyncControls() picks that up
						sync.setKnobRangeMs (ms);
						updateSyncControls();
					});
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

void TransportPanel::updateSyncControls (void)
{
	if (! syncAvailable)
	{
		return;
	}

	const bool enabled = sync.isEnabled();
	const double rangeMs = sync.getKnobRangeMs();
	const double offsetMs = sync.getCueOffsetMs();

	if (syncButton.getToggleState() != enabled)
	{
		syncButton.setToggleState (enabled, juce::dontSendNotification);
		// the accent glow reaches past the button bounds, and the nameplate on the frame changes
		// colour with it (see paintCardContent)
		repaint (buttonGlowBounds (syncButton.getBounds()));
		repaint (syncGroupBounds.expanded (kNameplateHeight / 2 + 2));
	}

	// The Cue button is deliberately never lit: a lit face in this editor means "this switch is on",
	// and Cue is an action (take the host's position as the anchor), not a switch.

	// Cue needs a host that reports time, and nothing else. The knob trims a cue that always
	// exists, so it is never disabled here - it has nothing to wait for.
	cueButton.setEnabled (sync.isHostUsable());

	const bool rangeChanged = ! juce::approximatelyEqual (rangeMs, lastKnobRangeMs);

	if (rangeChanged || ! juce::approximatelyEqual (offsetKnob.getValue(), offsetMs))
	{
		lastKnobRangeMs = rangeMs;

		// setRange() re-clamps the slider's value *and* notifies, which would echo the clamped
		// number back into the sync state through onValueChange
		updatingOffsetKnob = true;

		if (rangeChanged)
		{
			offsetKnob.setRange (-rangeMs, rangeMs, 0.0);
		}

		offsetKnob.setValue (offsetMs, juce::dontSendNotification);
		updatingOffsetKnob = false;
	}

	// The number is the value, so a re-clamp above is all that can make it stale (a no-op when the
	// string did not change - updateText() compares before it writes).
	offsetKnob.updateText();
}

} // namespace toybox::ui
