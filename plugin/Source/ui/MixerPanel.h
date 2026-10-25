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
	live in a Viewport; the master strip is pinned at the right of them and behaves like one of
	them: it grows with the card, keeps a strip's minimum height, and scrolls with the rows when
	the card cannot give it that much.

	The vertical scrollbar is the card's, not the strip column's - it sits at the far right of the
	card, outside the master, and scrolls the rows and the master together. (The viewport's own
	bar would land between the two columns, which reads as "this scrolls the strips".)

	Mixing happens in PlaybackEngine - the panel only forwards the user's changes and polls the
	lock-free values in refresh(), so the audio thread never reads anything from here.
*/
class MixerPanel : public CardComponent,
				   private juce::ScrollBar::Listener
{
public:
	/** Smallest height that still shows the header and a part of the first strip row - the
		editor reserves it when it hands out the space between the chips and transport cards. */
	static constexpr int minimumHeight = 178;

	explicit MixerPanel (toybox::PlaybackEngine& engineToUse);

	/** What the timer calls: a structure check plus the value updates - see the split below. */
	void refresh (void);

	/** Re-applies the card header, tooltips, "(not connected)" and the master name. */
	void refreshLanguage (void);

private:
	/** Rebuilds the strips when another song is loaded. Called from refresh(), which is why the
		per-tick path does nothing but compare a file name and a track count. */
	void syncStructure (void);

	/** Everything that changes while a song plays: the header buttons, the meters, the strips and
		their output selectors. Only reached when a song is loaded. */
	void refreshValues (void);

	/** Viewport content: hosts the strips and paints the group headers above them. */
	class StripHolder : public juce::Component
	{
	public:
		void paint (juce::Graphics& g) override;

		std::vector<std::pair<juce::String, juce::Rectangle<int>>> groupHeaders;
	};

	/** The row viewport, with one hook: the master strip is pinned outside it but scrolls with it
		vertically, so every scroll has to re-place the master (defined in the .cpp - it calls into
		the panel). */
	class StripViewport : public juce::Viewport
	{
	public:
		explicit StripViewport (MixerPanel& ownerToUse) : owner (ownerToUse) {}

		void visibleAreaChanged (const juce::Rectangle<int>& newVisibleArea) override;

	private:
		MixerPanel& owner;

		JUCE_DECLARE_NON_COPYABLE (StripViewport)
	};

	/** The master's clipping box. It hands the wheel to the row viewport: the master sits outside
		it, so scrolling over the master would otherwise do nothing while scrolling over the strips
		works - the two are one column of one console. */
	class MasterClip : public juce::Component
	{
	public:
		explicit MasterClip (juce::Viewport& viewportToUse) : viewport (viewportToUse) {}

		void mouseWheelMove (const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;

	private:
		juce::Viewport& viewport;

		JUCE_DECLARE_NON_COPYABLE (MasterClip)
	};

	void resized (void) override;
	void paintCardContent (juce::Graphics& g) override;

	void rebuildStrips (void);
	/** Lays the strips out in their groups and sizes the holder (call after any change). */
	void layoutStrips (void);
	/** Places the master strip inside its clip: level with the first row, as tall as all of them,
		and offset by the viewport's scroll position. Called by layoutStrips() and by every scroll. */
	void layoutMaster (void);
	/** Points the card's scrollbar at the viewport's state - shown only when the rows need more
		room than the card has, and following every scroll that does not come from the bar itself. */
	void syncScrollBar (int contentHeight);
	void scrollBarMoved (juce::ScrollBar* scrollBarThatHasMoved, double newRangeStart) override;
	/** Track n goes to bus n ("track 1 -> Out 1/2", "track 2 -> Out 3/4", ...). */
	void spreadTrackOutputs (void);
	/** Every track goes back to the internal master bus. */
	void sendTrackOutputsToMaster (void);

	toybox::PlaybackEngine& engine;
	IconButton mixerToggle { "mixer", "Mixer", IconButton::Icon::sliders };
	IconButton dcBlockButton { "dc-block", "Remove DC offset", IconButton::Icon::dcBlock };
	// the two routing shortcuts in the card header: spread every track over the output buses,
	// or send all of them back to the master bus
	IconButton spreadOutputsButton { "spread-outputs", "Spread outputs", IconButton::Icon::spreadOutputs };
	IconButton mergeToMasterButton { "send-to-master", "Send to master", IconButton::Icon::mergeToMaster };
	StripViewport viewport { *this };
	StripHolder holder;
	juce::OwnedArray<ChannelStrip> strips;
	MasterClip masterClip { viewport };	// clips the master strip; its bounds exclude the header row
	ChannelStrip masterStrip;
	juce::ScrollBar verticalBar { true };	// the card's own scrollbar, at the far right
	bool updatingScrollBar = false;			// the bar follows the viewport - do not feed it back

	juce::String loadedFile;			// detects "another song was loaded" for the rebuild
	int loadedTrackCount = -1;
	juce::Rectangle<int> masterColumn;	// set by resized(), the strip itself is placed by layoutMaster()
	int contentHeight = 0;				// the rows' extent (no trailing gap), set by layoutStrips()
	std::uint64_t lastOutputConnectivity = 0;	// rebuilds the selectors when the host's layout changes
	std::vector<float> meterLeft, meterRight;	// decayed peaks, one per track
	float masterPeakL = 0.0f;
	float masterPeakR = 0.0f;
	juce::Rectangle<int> headerBounds;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerPanel)
};

} // namespace toybox::ui
