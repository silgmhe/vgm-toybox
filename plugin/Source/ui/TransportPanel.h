#pragma once

#include "CardComponent.h"
#include "IconButton.h"

#include <functional>
#include <memory>

namespace toybox { class PlaybackEngine; }

namespace toybox::playback { class DawSync; }

namespace toybox::ui
{

class SyncGroup;	// ui/SyncGroup.h - the DAW SYNC cluster, created only in a hosted build

/**
	The transport card: play/pause/stop, the loop-forever toggle, the seek bar with the loop
	point marker and the playback start cursor above it, the DAW sync group, and the
	loop/fade/volume settings row.

	Owns every control that writes to the engine's transport API and pulls their state back in
	refresh(). The editor keeps no transport state of its own.

	DAW-flavoured transport: the playback start cursor is where Stop (and a finished song)
	returns to. Its marker sits in the strip above the seek bar and can be dragged. The
	cursor toggle next to the loop button switches the whole feature off, which leaves a
	plain player: no marker, Stop returns to the beginning of the song and space falls back
	to toggling play/pause.

	DAW sync (playback/DawSync.h) is one component, ui/SyncGroup - the framed SYNC / CUE / knob
	cluster at the right end of the button row - because its frame, its menu and the room it needs
	are one thing. The card creates it only in a hosted build and asks it two questions: refresh()
	and isHostDriving(). What the cue means is DawSync's; this card is the transport.
*/
class TransportPanel : public CardComponent
{
public:
	/** 52 (SyncGroup's frame) + 3 + 12 (marker strip) + 1 + 22 (slider) + 8 + 30 (settings)
		+ 2 * 12 (padding). The nameplate straddles the frame's top edge, so it needs no room of
		its own. */
	static constexpr int preferredHeight = 152;

	/** `syncAvailableToUse` is false in the standalone, which has no play head to follow - there
		the sync group is not created at all. */
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

	/** True while the host drives the transport - sync is on and the host reports time. The local
		transport is disabled then, and so is the space bar: the window stops consuming it and lets
		it out to the host, whose own transport shortcut runs instead (see ui/PlayView.cpp). */
	bool isHostDriving (void) const;

private:
	/** The number entry the sync group's knob range opens. An overlay inside the card rather than
		a dialog window: a plugin editor must not open one (AboutOverlay.h says what happened the
		one time this project did), and the card is big enough to hold three rows. Defined in the
		.cpp - it is card-sized, which is why it lives here and not in SyncGroup. */
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

	/** Opens the value entry over the card and hands the typed number to `onAccept`. SyncGroup
		asks for this through its onRequestValueEntry: the overlay covers the card, not the group. */
	void openValueEntry (const juce::String& title, double initialValue,
						 std::function<void (double)> onAccept);

	void paintPlaybackStartCursor (juce::Graphics& g) const;
	void movePlaybackStartToBandX (int bandX);
	void setPlaybackStart (double seconds);
	juce::Rectangle<int> playbackStartRepaintArea (double seconds) const;

	toybox::PlaybackEngine& engine;
	toybox::playback::DawSync& sync;
	std::unique_ptr<SyncGroup> syncGroup;	// null in the standalone: nothing to follow

	IconButton playButton { "Play", IconButton::Icon::play };
	IconButton pauseButton { "Pause", IconButton::Icon::pause };
	IconButton stopButton { "Stop", IconButton::Icon::stop };
	IconButton loopButton { "Loop", IconButton::Icon::loop };
	IconButton cursorButton { "Playback start", IconButton::Icon::playStart };
	juce::Slider positionSlider;
	juce::Slider volumeSlider;
	juce::ComboBox loopBox;
	juce::ComboBox fadeBox;

	StartMarkerDragArea startMarkerDragArea { *this };

	std::unique_ptr<ValueEntryOverlay> entryOverlay;	// created in the constructor, hidden

	juce::Rectangle<int> markerBandBounds;	// strip above the position slider (both markers)
	double loopMarkerNormalised = -1.0;		// < 0 = the song has no loop point to show
	juce::File lastFile;					// to reset the start cursor when another file loads
	double playbackStartSeconds = 0.0;		// where stop (and a finished song) returns to
	int lastLoopCount = -1;					// for the loop tooltip
	bool userIsDragging = false;
	bool startCursorEnabled = true;	// off = plain player: no marker, Stop returns to the beginning

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportPanel)
};

} // namespace toybox::ui
