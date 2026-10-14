#pragma once

#include "CardComponent.h"
#include "IconButton.h"

namespace vgmplay { class VgmEngine; }

namespace vgmplay::ui
{

/**
	The transport card: play/pause/stop, the loop-forever toggle, the seek bar with the loop
	point marker above it and the loop/fade/volume settings row.

	Owns every control that writes to the engine's transport API and pulls their state back in
	refresh(). The editor keeps no transport state of its own.
*/
class TransportPanel : public CardComponent
{
public:
	/** 34 (buttons) + 5 + 12 (loop marker) + 1 + 22 (slider) + 8 + 26 (settings) + 2 * 12 (padding). */
	static constexpr int preferredHeight = 132;

	explicit TransportPanel (vgmplay::VgmEngine& engineToUse);

	/** Pulls the transport state, the loop/fade setting and the position from the engine. */
	void refresh (void);

	/** Space key: plays while paused/stopped, pauses while playing. */
	void togglePlayPause (void);
	void seekRelative (double seconds);

private:
	void resized (void) override;
	void paintCardContent (juce::Graphics& g) override;

	void setLoopForever (bool shouldLoopForever);

	vgmplay::VgmEngine& engine;

	IconButton playButton { "Play", IconButton::Icon::play };
	IconButton pauseButton { "Pause", IconButton::Icon::pause };
	IconButton stopButton { "Stop", IconButton::Icon::stop };
	IconButton loopButton { "Loop", IconButton::Icon::loop };
	juce::Slider positionSlider;
	juce::Slider volumeSlider;
	juce::ComboBox loopBox;
	juce::ComboBox fadeBox;

	juce::Rectangle<int> loopMarkerBounds;	// strip above the position slider
	double loopMarkerNormalised = -1.0;		// < 0 = the song has no loop point to show
	int lastLoopCount = -1;					// for the loop tooltip
	bool userIsDragging = false;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportPanel)
};

} // namespace vgmplay::ui
