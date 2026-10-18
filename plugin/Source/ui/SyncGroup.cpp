#include "SyncGroup.h"

#include "ButtonPaint.h"
#include "Icons.h"
#include "Theme.h"

#include "i18n/LanguageManager.h"
#include "playback/DawSync.h"
#include "playback/PlaybackEngine.h"

#include <cmath>

namespace toybox::ui
{
namespace
{
	// The buttons reserve their own shadow room inside this (ButtonPaint.h), so the size here is
	// the face plus 2 * buttonShadowInset - the same 40 the transport row uses, which is what makes
	// the two rows line up.
	constexpr int kButtonSize = 40;

	// The cue trim: a rotary that fits the button row, and the number field beside it (the
	// Slider's own text box - the same box the volume shows its percentage in).
	constexpr int kOffsetKnobSize = 40;		// the rotary area; JUCE lays the text box out beside it
	constexpr int kOffsetValueWidth = 62;
	constexpr int kSyncGroupGap = 14;		// between the Cue button and the knob

	// A dashed frame drawn on the card: the controls are one cluster, and a dashed outline says
	// that without pretending to be a surface. The padding is generous on purpose - a frame the
	// controls touch reads as a selection rectangle drawn around them. The nameplate straddles the
	// top edge, so only its lower half has to clear the controls.
	constexpr int kFramePadding = 8;		// left and right, inside the frame
	constexpr int kFrameTopPadding = 9;		// from the frame's top edge down to the controls
	constexpr int kFrameBottomPadding = 3;

	// 3 px dashes, 3 off: fine enough to read as a guide rather than as a border
	constexpr float kFrameDash = 3.0f;
	constexpr float kFrameGap = 3.0f;

	// the nameplate: 8 px caps, tracked out, sitting on the frame's top edge
	constexpr float kNameplateTracking = 0.5f;	// extra pixels between the glyphs
	constexpr int kNameplatePadding = 6;		// left and right of the text, inside the gap
	constexpr int kNameplateHeight = 10;		// the gap cut into the dashes behind the text

	/** The offset as the knob's number shows it. It is the offset and nothing else: the cue always
		exists, so the field has no "no cue" case to fall back to. */
	juce::String offsetText (double ms)
	{
		const double shown = (std::abs (ms) < 0.05) ? 0.0 : ms;	// never "-0.0"
		return (shown > 0.0 ? "+" : "") + juce::String (shown, 1) + " ms";
	}

	/** The dashed frame around the group: 1 px dashes along a rounded rectangle, and nothing else -
		no fill, no inner shadow, no bevelled edge. A recess has to argue that it is structure and
		not a selection rectangle, and at this size that argument cannot be won; a dashed line only
		ever means "these belong together", so it does not have to make it.

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

	/** The nameplate of the group: 8 px bold caps, tracked out letter by letter, sitting *on* the
		frame's top edge with the dashes erased behind it - the legend of a fieldset, and how a
		frame that carries a name is drawn everywhere else.

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

SyncGroup::SyncGroup (toybox::PlaybackEngine& engineToUse)
	: engine (engineToUse),
	  sync (engineToUse.getDawSync())
{
	setComponentID ("group.sync");	// for the window scripts (AppHarnessTest)


	// The toggle reads the sync state in refresh() and turns over what it shows, so the button
	// itself stays the one place the state is shown. Both glyphs carry their word: a chain link
	// or a flag alone does not say "this follows your DAW".
	syncButton.onClick = [this]
	{
		sync.setEnabled (! sync.isEnabled());
		refresh();

		// The card's transport is gated by this switch, so it has to re-read it now rather than
		// on its next tick.
		if (onStateChanged != nullptr)
		{
			onStateChanged();
		}
	};
	addAndMakeVisible (syncButton);

	// Taking the cue is an action, not a toggle and not a state light: the DAW moves, so taking it
	// again is the normal gesture. The button is therefore never lit - a lit face in this editor
	// means "this switch is on", and Cue is not a switch. There is no "has a cue" state to show
	// either: the cue always exists (0 is the DAW's start), and the menu can put it back there.
	cueButton.onClick = [this] { takeCue(); };
	addAndMakeVisible (cueButton);

	offsetKnob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
	offsetKnob.setTextBoxStyle (juce::Slider::TextBoxRight, false, kOffsetValueWidth, 22);
	offsetKnob.setComponentID ("knob.cue-offset");	// for the window scripts (AppHarnessTest)
	offsetKnob.setRange (-lastKnobRangeMs, lastKnobRangeMs, 0.0);
	offsetKnob.setValue (0.0, juce::dontSendNotification);
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
	addAndMakeVisible (offsetKnob);

	// A left click on a control is untouched (a Button ignores a right click already, and the knob
	// swallows it), so the listener only has to catch the right ones.
	addMouseListener (&childRightClick, true);

	lookAndFeelChanged();
	refreshLanguage();
}

void SyncGroup::refreshLanguage (void)
{
	syncButton.setName (i18n::tr ("sync.name"));
	syncButton.setCaption (i18n::tr ("sync.caption"));
	syncButton.setTooltip (i18n::tr ("sync.tooltip"));
	cueButton.setName (i18n::tr ("sync.cueName"));
	cueButton.setCaption (i18n::tr ("sync.cueCaption"));
	cueButton.setTooltip (i18n::tr ("sync.cueTooltip"));
	offsetKnob.setTooltip (i18n::tr ("sync.offsetTooltip"));
	repaint();
}

SyncGroup::~SyncGroup (void)
{
	// The listener is a member, so it dies before the base class does: unregister it while both
	// are still alive.
	removeMouseListener (&childRightClick);
}

int SyncGroup::preferredWidth (void)
{
	return 2 * kFramePadding + kButtonSize + 6 + kButtonSize + kSyncGroupGap + kOffsetKnobSize
		   + kOffsetValueWidth;
}

int SyncGroup::frameHeight (void)
{
	return kFrameTopPadding + kButtonSize + kFrameBottomPadding;
}

int SyncGroup::nameplateOverhang (void)
{
	return kNameplateHeight / 2;
}

juce::Rectangle<int> SyncGroup::frameBounds (void) const
{
	return getLocalBounds().withTrimmedTop (kNameplateHeight / 2);
}

void SyncGroup::resized (void)
{
	auto inner = frameBounds().withTrimmedLeft (kFramePadding)
							  .withTrimmedRight (kFramePadding)
							  .withTrimmedTop (kFrameTopPadding)
							  .withTrimmedBottom (kFrameBottomPadding);

	syncButton.setBounds (inner.removeFromLeft (kButtonSize));
	inner.removeFromLeft (6);
	cueButton.setBounds (inner.removeFromLeft (kButtonSize));
	inner.removeFromLeft (kSyncGroupGap);
	offsetKnob.setBounds (inner);	// the rotary, and its number field, to the right of it
}

void SyncGroup::paint (juce::Graphics& g)
{
	const auto frame = frameBounds();
	const float frameRadius = theme::cardRadius - 2.0f;

	// The frame never changes colour - the version of this that lit the whole outline read as a
	// selection rectangle drawn around the group. Whether the group is live is said by the switch
	// inside it (accent face and glow) and by the nameplate: the thing is lit, not boxed.

	// The sync button's glow is clipped to the frame: the frame is the box the light is in, and an
	// unclipped halo would wash over the dashes and onto the card around them.
	if (syncButton.getToggleState())
	{
		juce::Graphics::ScopedSaveState save (g);

		juce::Path clip;
		clip.addRoundedRectangle (frame.toFloat(), frameRadius);
		g.reduceClipRegion (clip);

		drawButtonGlow (g, syncButton.getBounds());
	}

	// After the glow: the frame closes around the light instead of being tinted by it
	drawGroupFrame (g, frame.toFloat(), frameRadius, theme::groupFrame());

	// the nameplate is the one thing in the group that does carry the state
	drawGroupNameplate (g, i18n::tr ("sync.nameplate"), frame,
						sync.isEnabled() ? theme::accent() : theme::secondaryText());
}

void SyncGroup::lookAndFeelChanged (void)
{
	// The number field's colours have to be set on the slider itself, because that is where the
	// box's label copies them from when it is created - and the box is recreated whenever the
	// LookAndFeel changes (Slider::colourChanged() forwards to Slider::lookAndFeelChanged()), so
	// re-applying them here is what keeps the number readable after a theme switch.
	offsetKnob.setColour (juce::Slider::textBoxTextColourId, theme::primaryText());
	// Must be opaque: JUCE's createSliderTextBox() copies this colour into the *editor* that
	// appears while typing and forces alpha to 1, so a transparent black would turn into an opaque
	// black box with dark text in it.
	offsetKnob.setColour (juce::Slider::textBoxBackgroundColourId, theme::card());
	offsetKnob.setColour (juce::Slider::textBoxOutlineColourId, theme::border());
	offsetKnob.setColour (juce::Slider::textBoxHighlightColourId, theme::accent().withAlpha (0.25f));
}

void SyncGroup::refresh (void)
{
	const bool enabled = sync.isEnabled();
	const double rangeMs = sync.getKnobRangeMs();
	const double offsetMs = sync.getCueOffsetMs();

	if (syncButton.getToggleState() != enabled)
	{
		syncButton.setToggleState (enabled, juce::dontSendNotification);
		// the accent glow reaches past the button bounds, and the nameplate on the frame changes
		// colour with it - both are this component's own paint
		repaint();
	}

	// The Cue button is deliberately never lit: a lit face in this editor means "this switch is
	// on", and Cue is an action (take the host's position as the anchor), not a switch.

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

bool SyncGroup::isHostDriving (void) const
{
	// The group only exists in a hosted build (the card creates it from its wrapper type), so
	// there is no separate "syncAvailable" to check here.
	return sync.isActive();
}

void SyncGroup::takeCue (void)
{
	// Always takes: the cue is where the song starts on the DAW timeline, and every click re-anchors
	// it there. It is never cleared - the menu can put it back to 0 s (the DAW's start), which is a
	// cue like any other.
	// setCueFromHost() returns false when the host has no time to give; the button is disabled then,
	// so that is only a race with a host that just stopped reporting.
	sync.setCueFromHost();
	refresh();
}

void SyncGroup::mouseDown (const juce::MouseEvent& event)
{
	openMenuFor (event);
}

void SyncGroup::openMenuFor (const juce::MouseEvent& event)
{
	if (! event.mods.isPopupMenu())
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

	// At the click, not at the knob: any point in the frame could have asked for this.
	showMenu (event.getScreenPosition());
}

void SyncGroup::showMenu (juce::Point<int> screenPosition)
{
	juce::PopupMenu menu;
	menu.addItem (1, i18n::tr ("sync.menu.exactOffset"));
	menu.addItem (2, i18n::tr ("sync.menu.knobRange"));
	menu.addSeparator();
	menu.addItem (3, i18n::tr ("sync.menu.resetCue"));

	// "Exact offset" opens the number field itself (Slider::showTextBox) rather than an overlay:
	// the field is the same edit box the volume uses, so there is one way to type a number, and
	// the menu item is only the discoverable path to it.

	// The menu has to be told this LookAndFeel: PopupMenu only looks at its *own* pointer
	// (menu.lookAndFeel, PopupMenu::setLookAndFeel), never at the target component's, so without
	// this line the menu is drawn by whatever the platform's default LookAndFeel is - the default
	// dark V4 palette over a light card, which is what "the colours do not follow the theme" was.
	// The editor owns the object for the whole time the menu can live.
	menu.setLookAndFeel (&getLookAndFeel());

	// The menu outlives the click that opened it and so does its callback: the group is tracked
	// rather than captured, because closing the editor destroys it.
	const juce::Component::SafePointer<SyncGroup> safe (this);

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
								safe->refresh();
							}
						});
}

void SyncGroup::showRangeEntry (void)
{
	if (onRequestValueEntry == nullptr)
	{
		return;
	}

	const juce::Component::SafePointer<SyncGroup> safe (this);

	onRequestValueEntry (i18n::tr ("sync.knobRangeTitle"), sync.getKnobRangeMs(),
						 [safe] (double ms)
						 {
							 if (safe == nullptr)
							 {
								 return;
							 }

							 // setKnobRangeMs() re-clamps the offset, so the knob can never show a
							 // value it cannot reach - refresh() picks that up
							 safe->sync.setKnobRangeMs (ms);
							 safe->refresh();
						 });
}

} // namespace toybox::ui
