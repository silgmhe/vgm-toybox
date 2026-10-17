#pragma once

#include "ChipPanel.h"
#include "MixerPanel.h"
#include "SongInfoPanel.h"
#include "TransportPanel.h"
#include "View.h"

namespace toybox { class PlaybackEngine; }

namespace toybox::ui
{

/**
	The playback page: everything the window shows while it plays - the song info card, the sound
	chip mutes, the mixer and the transport.

	It was the whole window until the View menu arrived; the panels, their layout, their refresh
	and the transport keys moved here unchanged, so the window itself could shrink to what is
	genuinely global to it (the menu bar, the open button, the status line, the file drops).
*/
class PlayView : public View
{
public:
	/** `syncAvailable` is forwarded to the transport card: only a hosted build has a play head to
		follow (see TransportPanel). */
	explicit PlayView (toybox::PlaybackEngine& engine, bool syncAvailable = true);

	juce::String getViewName (void) const override { return "Play"; }

	void paint (juce::Graphics& g) override;
	void resized (void) override;
	void refresh (void) override;
	void songChanged (void) override;
	bool handleKey (const juce::KeyPress& key) override;

private:
	/** The chip checkboxes belong to the song, so this runs whenever there is a different one.
		It relayouts as well: the card can appear, disappear or change height. */
	void rebuildChipButtons (void);

	toybox::PlaybackEngine& engine;

	SongInfoPanel songInfoPanel;
	TransportPanel transportPanel;
	ChipPanel chipPanel;
	MixerPanel mixerPanel;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlayView)
};

} // namespace toybox::ui
