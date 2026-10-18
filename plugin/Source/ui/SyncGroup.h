#pragma once

#include "IconButton.h"

#include <functional>

namespace toybox { class PlaybackEngine; }

namespace toybox::playback { class DawSync; }

namespace toybox::ui
{

/**
	The DAW SYNC cluster: the Sync toggle, the Cue action, the trim knob with its number beside it,
	and the dashed frame that holds all three.

	A component of its own rather than three controls the transport card lays out: the frame, the
	glow it clips, the knob's right-click menu and the size the row has to leave for it are one
	thing, and everything the card needs to know about them is `preferredWidth()` and
	`isHostDriving()`. It is created only in a hosted build - the standalone has no play head to
	follow, so there is nothing here to show (docs/desgin/daw-sync-cue-design.md §7.4).

	What the cue *means* is not here: that is playback/DawSync.h. This is the controls, and the
	only state they keep is "what the knob was last told" (see refresh()).

	Its menu asks the card to open the number entry (`onRequestValueEntry`) because that overlay
	covers the whole card, which is bigger than this group.
*/
class SyncGroup : public juce::Component
{
public:
	explicit SyncGroup (toybox::PlaybackEngine& engineToUse);
	~SyncGroup (void) override;

	/** The width the group needs in the card's button row, the height of its frame (the card's
		row is that tall in the standalone too, so the transport does not jump between builds),
		and how far the nameplate reaches above the frame - the card has to leave that much room. */
	static int preferredWidth (void);
	static int frameHeight (void);
	static int nameplateOverhang (void);

	/** Pulls the cue, the offset and the knob's range into the controls. */
	void refresh (void);

	/** Re-applies captions, tooltips, the nameplate and the right-click menu's wording. */
	void refreshLanguage (void);

	/** True while the host drives the transport - sync is on and the host reports time. The card
		asks this to disable its own transport and to let the space bar out to the host. */
	bool isHostDriving (void) const;

	/** Sync was toggled or a cue was taken: the card re-reads what the host owns from that. */
	std::function<void()> onStateChanged;

	/** The card's value entry (the knob's range), opened over the card rather than over this
		group - the overlay is card-sized, so the card is what can show it. */
	std::function<void (const juce::String& title, double initialValue,
						std::function<void (double)> onAccept)>
		onRequestValueEntry;

	void resized (void) override;
	void paint (juce::Graphics& g) override;
	void mouseDown (const juce::MouseEvent& event) override;

	/** The number beside the knob is a text box inside the slider, and its colours are copies -
		a theme change has to re-apply them (the card does the same for its volume box). */
	void lookAndFeelChanged (void) override;

private:
	/** The cue-offset knob: a rotary slider whose number (the Slider's own text box, to the right
		of the face) is the exact-value field - the way the volume's "100 %" box is - and whose
		double click snaps the offset back to zero, which is the only value a drag cannot be relied
		on to hit. A right click is the group's, not the knob's: the menu belongs to the frame. */
	class OffsetKnob : public juce::Slider
	{
	public:
		void mouseDown (const juce::MouseEvent& event) override
		{
			// The menu belongs to the group, so a right click here must not reach the slider's
			// own popup menu.
			if (event.mods.isPopupMenu())
			{
				return;
			}

			juce::Slider::mouseDown (event);
		}

		void mouseDoubleClick (const juce::MouseEvent& event) override
		{
			juce::ignoreUnused (event);

			// Reset - by hand a knob that small cannot be dragged to an exact 0.0, and "put it
			// back" is the one gesture a trim control needs. The notification is what carries it
			// into the sync state (onValueChange), exactly like a drag.
			setValue (0.0, juce::sendNotificationSync);
		}
	};

	/** Right clicks anywhere in the frame. A listener rather than the group's own mouseDown: the
		buttons and the knob cover most of the frame, and a right click on any of them has to reach
		the same menu. */
	class ChildRightClick : public juce::MouseListener
	{
	public:
		explicit ChildRightClick (SyncGroup& ownerToUse) : owner (ownerToUse) {}

		void mouseDown (const juce::MouseEvent& event) override { owner.openMenuFor (event); }

	private:
		SyncGroup& owner;

		JUCE_DECLARE_NON_COPYABLE (ChildRightClick)
	};

	/** The frame, in this component's coordinates: the nameplate straddles its top edge, which is
		why the component is taller than the frame. */
	juce::Rectangle<int> frameBounds (void) const;

	void takeCue (void);
	void openMenuFor (const juce::MouseEvent& event);
	void showMenu (juce::Point<int> screenPosition);
	void showRangeEntry (void);

	toybox::PlaybackEngine& engine;
	toybox::playback::DawSync& sync;

	IconButton syncButton { "sync", "Sync", IconButton::Icon::sync };
	IconButton cueButton { "cue", "Cue", IconButton::Icon::cue };
	OffsetKnob offsetKnob;

	ChildRightClick childRightClick { *this };	// registered in the constructor

	double lastKnobRangeMs = 10.0;	// to rewrite the knob's range only when it changed
	bool updatingOffsetKnob = false;	// refresh() writes the knob - do not echo it back

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SyncGroup)
};

} // namespace toybox::ui
