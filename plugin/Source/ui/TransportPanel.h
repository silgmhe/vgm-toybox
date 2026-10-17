#pragma once

#include "CardComponent.h"
#include "IconButton.h"

#include <functional>
#include <memory>

namespace toybox { class PlaybackEngine; }

namespace toybox::playback { class DawSync; }

namespace toybox::ui
{

/**
	The transport card: play/pause/stop, the loop-forever toggle, the seek bar with the loop
	point marker and the playback start cursor above it, and the loop/fade/volume settings row.

	Owns every control that writes to the engine's transport API and pulls their state back in
	refresh(). The editor keeps no transport state of its own.

	DAW-flavoured transport: the playback start cursor is where Stop (and a finished song)
	returns to. Its marker sits in the strip above the seek bar and can be dragged. The
	cursor toggle next to the loop button switches the whole feature off, which leaves a
	plain player: no marker, Stop returns to the beginning of the song and space falls back
	to toggling play/pause.

	DAW sync (playback/DawSync.h) sits in one framed group at the right end of the button row,
	labelled DAW SYNC: the Sync toggle makes the song follow the host's clock - the position is
	"host position - cue", nothing else - the Cue button takes the host's position as the point
	where the song's zero plays, and the knob beside it trims that cue by a few milliseconds.

	The cue is a stored anchor that always exists - 0 (the DAW's start) is its default, and the
	frame's right-click menu can put it back there. There is no "has a cue" state: Sync is the only
	switch, and when it is on with a usable host the mapping is live. Clicking Cue only ever takes
	the anchor (again) and its button is never lit - a lit face in this editor means "this switch is
	on", and Cue is an action. The knob is dragged, its number beside it is the exact offset field
	(the same box as the volume's "100 %"), and a double click on the face puts the offset back to
	zero. All of it is one-way: the song follows the host and never drives it.
*/
class TransportPanel : public CardComponent
{
public:
	/** 52 (DAW-sync frame) + 3 + 12 (marker strip) + 1 + 22 (slider) + 8 + 30 (settings)
		+ 2 * 12 (padding), see the kFrame* constants in the .cpp. The nameplate straddles the
		frame's top edge, so it needs no room of its own. */
	static constexpr int preferredHeight = 152;

	/** `syncAvailableToUse` is false in the standalone, which has no play head to follow - there
		the two sync buttons and the trim knob are not created at all. */
	explicit TransportPanel (toybox::PlaybackEngine& engineToUse, bool syncAvailableToUse = true);
	~TransportPanel (void) override;

	/** Pulls the transport state, the loop/fade setting and the position from the engine. */
	void refresh (void);

	/** Space key: with the start cursor on, stops (back to the cursor) while playing and
		plays otherwise; with it off, toggles play/pause like a media player. */
	void togglePlayStop (void);

	/** Enter key: pauses (staying put) while playing, plays otherwise. */
	void togglePlayPause (void);
	void seekRelative (double seconds);

	/** Seeks the transport; while it is not running, the playback start cursor follows. */
	void seekToSeconds (double seconds);

private:
	/** The cue-offset knob: a rotary slider whose number (the Slider's own text box, to the right of
		the face) is the exact-value field - the way the volume's "100 %" box is - and whose double
		click snaps the offset back to zero, which is the only value the drag cannot be relied on to
		hit. A right click is the group's, not the knob's: the panel's listener opens the menu for
		the whole DAW SYNC frame. */
	class OffsetKnob : public juce::Slider
	{
	public:
		void mouseDown (const juce::MouseEvent& event) override
		{
			// The menu belongs to the group (the panel's listener opens it), so a right click here
			// must not reach the slider's own popup menu.
			if (event.mods.isPopupMenu())
			{
				return;
			}

			juce::Slider::mouseDown (event);
		}

		void mouseDoubleClick (const juce::MouseEvent& event) override
		{
			juce::ignoreUnused (event);

			// Reset - by hand a knob that small cannot be dragged to an exact 0.0, and "put it back"
			// is the one gesture a trim control needs. The notification is what carries it into the
			// sync state (the panel's onValueChange), exactly like a drag.
			setValue (0.0, juce::sendNotificationSync);
		}
	};

	/** Right clicks inside the DAW SYNC frame. A listener rather than the panel's own mouseDown:
		only part of the frame is bare panel, because the two buttons and the knob sit on top of
		it, and a right click on any of those has to reach the same menu. */
	class SyncGroupMouseListener : public juce::MouseListener
	{
	public:
		explicit SyncGroupMouseListener (TransportPanel& ownerToUse) : owner (ownerToUse) {}

		void mouseDown (const juce::MouseEvent& event) override { owner.syncGroupMouseDown (event); }

	private:
		TransportPanel& owner;

		JUCE_DECLARE_NON_COPYABLE (SyncGroupMouseListener)
	};

	/** The number entry the knob opens (the offset, or the knob's range). An overlay inside the
		card rather than a dialog window: a plugin editor must not open one (AboutOverlay.h says
		what happened the one time this project did), and the card is big enough to hold three
		rows. Defined in the .cpp. */
	class ValueEntryOverlay;

	/** Transparent strip above the seek bar; dragging it moves the playback start cursor. */
	class StartMarkerDragArea : public juce::Component,
								public juce::SettableTooltipClient
	{
	public:
		explicit StartMarkerDragArea (TransportPanel& ownerToUse) : owner (ownerToUse) {}

	private:
		void mouseDown (const juce::MouseEvent& event) override { owner.movePlaybackStartToBandX (event.x); }
		void mouseDrag (const juce::MouseEvent& event) override { owner.movePlaybackStartToBandX (event.x); }

		TransportPanel& owner;

		JUCE_DECLARE_NON_COPYABLE (StartMarkerDragArea)
	};

	void resized (void) override;
	void paintCardContent (juce::Graphics& g) override;

	/** Re-applies the colours the panel sets on its own controls (setColour stores a copy, so a
		theme change does not reach them by itself - see PluginEditor::lookAndFeelChanged). */
	void lookAndFeelChanged (void) override;

	void startPlayback (void);
	void stopTransport (void);
	void pausePlayback (void);
	void setLoopForever (bool shouldLoopForever);
	void setStartCursorEnabled (bool shouldBeEnabled);

	/** The Cue button: takes the host's position as the cue. Always takes - re-taking is the
		normal gesture (the DAW moved, or the song changed), resetting is on the frame's menu. */
	void takeCue (void);
	/** The DAW SYNC frame's right-click menu: the exact offset (which opens the number field), how
		far the knob travels and resetting the cue to 0 s. Opened from anywhere inside the frame,
		so it is the group's menu rather than the knob's. */
	void showOffsetMenu (juce::Point<int> screenPosition);
	/** The panel's half of SyncGroupMouseListener: opens the menu for a right click anywhere in
		the frame, including on its controls. */
	void syncGroupMouseDown (const juce::MouseEvent& event);
	/** The number entry the menu opens for the knob's range. */
	void showRangeEntry (void);
	/** Opens the value entry over the card and hands the typed number to `onAccept`. */
	void openValueEntry (const juce::String& title, double initialValue,
						 std::function<void (double)> onAccept);
	/** Pulls the cue/offset/range state into the buttons, the knob's number and the group frame. */
	void updateSyncControls (void);

	void paintPlaybackStartCursor (juce::Graphics& g) const;
	void movePlaybackStartToBandX (int bandX);
	void setPlaybackStart (double seconds);
	juce::Rectangle<int> playbackStartRepaintArea (double seconds) const;

	toybox::PlaybackEngine& engine;
	toybox::playback::DawSync& sync;
	const bool syncAvailable;

	IconButton playButton { "Play", IconButton::Icon::play };
	IconButton pauseButton { "Pause", IconButton::Icon::pause };
	IconButton stopButton { "Stop", IconButton::Icon::stop };
	IconButton loopButton { "Loop", IconButton::Icon::loop };
	IconButton cursorButton { "Playback start", IconButton::Icon::playStart };
	IconButton syncButton { "Sync", IconButton::Icon::sync };
	IconButton cueButton { "Cue", IconButton::Icon::cue };
	OffsetKnob offsetKnob;
	juce::Slider positionSlider;
	juce::Slider volumeSlider;
	juce::ComboBox loopBox;
	juce::ComboBox fadeBox;

	StartMarkerDragArea startMarkerDragArea { *this };
	SyncGroupMouseListener syncGroupMouseListener { *this };	// registered in the constructor

	std::unique_ptr<ValueEntryOverlay> entryOverlay;	// created in the constructor, hidden

	juce::Rectangle<int> markerBandBounds;	// strip above the position slider (both markers)
	juce::Rectangle<int> syncGroupBounds;	// the DAW SYNC frame, painted by paintCardContent()
	double loopMarkerNormalised = -1.0;		// < 0 = the song has no loop point to show
	juce::File lastFile;					// to reset the start cursor when another file loads
	double playbackStartSeconds = 0.0;		// where stop (and a finished song) returns to
	double lastKnobRangeMs = 10.0;			// to rewrite the knob's range only when it changed
	int lastLoopCount = -1;					// for the loop tooltip
	bool userIsDragging = false;
	bool updatingOffsetKnob = false;		// the refresh writes the knob - do not echo it back
	bool paused = false;			// the last stop-like action was "pause" (not "stop") - drives the button lights
	bool startCursorEnabled = true;	// off = plain player: no marker, Stop returns to the beginning

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportPanel)
};

} // namespace toybox::ui
