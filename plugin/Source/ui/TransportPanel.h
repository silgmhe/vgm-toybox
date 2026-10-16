#pragma once

#include "CardComponent.h"
#include "IconButton.h"

namespace toybox { class PlaybackEngine; }

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
*/
class TransportPanel : public CardComponent
{
public:
	/** 40 (buttons) + 5 + 12 (marker strip) + 1 + 22 (slider) + 8 + 26 (settings) + 2 * 12 (padding). */
	static constexpr int preferredHeight = 138;

	explicit TransportPanel (toybox::PlaybackEngine& engineToUse);

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

	void startPlayback (void);
	void stopTransport (void);
	void pausePlayback (void);
	void setLoopForever (bool shouldLoopForever);
	void setStartCursorEnabled (bool shouldBeEnabled);

	void paintPlaybackStartCursor (juce::Graphics& g) const;
	void movePlaybackStartToBandX (int bandX);
	void setPlaybackStart (double seconds);
	juce::Rectangle<int> playbackStartRepaintArea (double seconds) const;

	toybox::PlaybackEngine& engine;

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

	juce::Rectangle<int> markerBandBounds;	// strip above the position slider (both markers)
	double loopMarkerNormalised = -1.0;		// < 0 = the song has no loop point to show
	juce::File lastFile;					// to reset the start cursor when another file loads
	double playbackStartSeconds = 0.0;		// where stop (and a finished song) returns to
	int lastLoopCount = -1;					// for the loop tooltip
	bool userIsDragging = false;
	bool paused = false;			// the last stop-like action was "pause" (not "stop") - drives the button lights
	bool startCursorEnabled = true;	// off = plain player: no marker, Stop returns to the beginning

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportPanel)
};

} // namespace toybox::ui
