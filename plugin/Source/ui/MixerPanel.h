#pragma once

#include <utility>
#include <vector>

#include "CardComponent.h"
#include "ChannelStrip.h"
#include "IconButton.h"

#include "playback/PlaybackEngine.h"

namespace toybox::ui
{

/**
	The mixer card below the chips card: one strip per chip channel, grouped by chip. The strips
	live in a Viewport (with horizontal and vertical scrollbars), the master strip stays pinned
	at the right.

	Mixing happens in PlaybackEngine - the panel only forwards the user's changes and polls the
	lock-free values in refresh(), so the audio thread never reads anything from here.
*/
class MixerPanel : public CardComponent
{
public:
	/** Smallest height that still shows the header and a part of the first strip row - the
		editor reserves it when it hands out the space between the chips and transport cards. */
	static constexpr int minimumHeight = 166;

	explicit MixerPanel (toybox::PlaybackEngine& engineToUse);

	/** Rebuilds the strips when another song is loaded and refreshes values and meters. */
	void refresh (void);

private:
	/** Viewport content: hosts the strips and paints the group headers above them. */
	class StripHolder : public juce::Component
	{
	public:
		void paint (juce::Graphics& g) override;

		std::vector<std::pair<juce::String, juce::Rectangle<int>>> groupHeaders;
	};

	void resized (void) override;
	void paintCardContent (juce::Graphics& g) override;

	void rebuildStrips (void);
	/** Lays the strips out in their groups and sizes the holder (call after any change). */
	void layoutStrips (void);
	/** Track n goes to bus n ("track 1 -> Out 1/2", "track 2 -> Out 3/4", ...). */
	void spreadTrackOutputs (void);
	/** Every track goes back to the internal master bus. */
	void sendTrackOutputsToMaster (void);

	toybox::PlaybackEngine& engine;
	IconButton mixerToggle { "Mixer", IconButton::Icon::sliders };
	// the two routing shortcuts in the card header: spread every track over the output buses,
	// or send all of them back to the master bus
	IconButton spreadOutputsButton { "Spread outputs", IconButton::Icon::spreadOutputs };
	IconButton mergeToMasterButton { "Send to master", IconButton::Icon::mergeToMaster };
	juce::Viewport viewport;
	StripHolder holder;
	juce::OwnedArray<ChannelStrip> strips;
	ChannelStrip masterStrip;

	juce::String loadedFile;			// detects "another song was loaded" for the rebuild
	int loadedTrackCount = -1;
	juce::Rectangle<int> masterColumn;	// set by resized(), the strip itself is placed by layoutStrips()
	std::uint64_t lastOutputConnectivity = 0;	// rebuilds the selectors when the host's layout changes
	std::vector<float> meterLeft, meterRight;	// decayed peaks, one per track
	float masterPeakL = 0.0f;
	float masterPeakR = 0.0f;
	juce::Rectangle<int> headerBounds;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerPanel)
};

} // namespace toybox::ui
