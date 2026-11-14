#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

namespace toybox::ui
{

/**
	A ComboBox whose selection is mirrored from the engine by a timer.

	**The bug this exists for.** Every selector in the editor had two writers: the user, whose pick
	reaches `onChange` through a *queued* `ChangeBroadcaster` message, and the owner's refresh timer,
	which read the engine and wrote the box back *synchronously*. When the timer ran between the pick
	and the delivery of its notification, it wrote the value the engine still held - the one the user
	had just replaced - and the choice was undone before it was ever applied. It is a phase race, so
	it hit roughly one pick in three and looked like "the dropdown sometimes does nothing".

	Two rules, both in this class so that no selector has to remember them:

	- a pick is applied with `sendNotificationSync`, so the engine holds the new value by the time
	  `setSelectedId` returns and the next refresh has nothing to undo;
	- a refresh that arrives while a pick is still in flight (`syncTo`'s grace window, and `holdOff`
	  for engines that build asynchronously) leaves the box alone.

	A selector with no custom popup gets a plain text menu built from its own items, which is what
	`ComboBox` would have shown anyway.
*/
class SyncedComboBox : public juce::ComboBox
{
public:
	/** Writes `itemId` into the box, unless a pick is still travelling - see the class comment. */
	void syncTo (int itemId);

	/** Writes `itemId` unconditionally. For a caller that has just rebuilt the item list and has to
		leave the box on a valid entry; the grace window does not apply, because an empty box is
		worse than a briefly wrong one. */
	void forceTo (int itemId);

	/** Records a pick and applies it: stamps the grace window, then selects with a synchronous
		notification so the engine holds the value before `syncTo` can look at it.

		`showPopup()` calls this, and so does the test - a popup is a window, and the race being
		tested is not.
	*/
	void noteUserPick (int itemId);

	/** Builds the popup. Empty: a plain menu from the box's own items. Called with the current
		selection so a custom menu can tick it. */
	std::function<juce::PopupMenu (int selectedItemId)> makeMenu;

	/** A veto for an engine whose value arrives asynchronously - a song switch builds in the
		background, so `getSongIndex()` keeps reporting the old song for hundreds of milliseconds and
		no grace window is long enough. `syncTo` leaves the box alone while this returns true. */
	std::function<bool (void)> holdOff;

	/** Test seam: the pick's timestamp comes from here. Defaults to the wall clock. */
	std::function<juce::uint32 (void)> clock = [] { return juce::Time::currentTimeMillis (); };

	void showPopup (void) override;

	/** How long a pick is assumed to still be travelling. Public so a test can step over it. */
	static constexpr juce::uint32 pickGraceMs = 400;

private:
	juce::PopupMenu buildDefaultMenu (int selectedItemId) const;

	bool pickInFlight = false;
	juce::uint32 lastPickMs = 0;
};

} // namespace toybox::ui
