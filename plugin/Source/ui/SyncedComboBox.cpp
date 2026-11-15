#include "SyncedComboBox.h"

namespace toybox::ui
{

void SyncedComboBox::syncTo (int itemId)
{
	if (holdOff != nullptr && holdOff ())
	{
		return;
	}

	const auto now = clock ();

	// The pick is still on its way to the engine (queued notification, or a write the engine has
	// not finished). Overwriting now would put back exactly the value the user just replaced.
	if (pickInFlight && (now - lastPickMs) < pickGraceMs)
	{
		return;
	}

	pickInFlight = false;

	if (getSelectedId () != itemId)
	{
		setSelectedId (itemId, juce::dontSendNotification);
	}
}

void SyncedComboBox::forceTo (int itemId)
{
	pickInFlight = false;

	if (getSelectedId () != itemId)
	{
		setSelectedId (itemId, juce::dontSendNotification);
	}
}

void SyncedComboBox::noteUserPick (int itemId)
{
	pickInFlight = true;
	lastPickMs = clock ();

	// **Synchronous, and that is the fix.** The owner's timer reads the engine and writes the box
	// back; with the default queued notification it ran first, saw the engine still holding the
	// previous value, and undid the pick before `onChange` had ever been delivered.
	setSelectedId (itemId, juce::sendNotificationSync);
}

juce::PopupMenu SyncedComboBox::buildDefaultMenu (int selectedItemId) const
{
	juce::PopupMenu menu;

	for (int i = 0; i < getNumItems (); ++i)
	{
		const int itemId = getItemId (i);
		menu.addItem (itemId, getItemText (i), true, itemId == selectedItemId);
	}

	return menu;
}

void SyncedComboBox::showPopup ()
{
	if (! isEnabled () || getNumItems () <= 0)
	{
		hidePopup ();	// hand back the "a menu is open" state ComboBox set before calling us
		return;
	}

	auto menu = (makeMenu != nullptr) ? makeMenu (getSelectedId ())
									  : buildDefaultMenu (getSelectedId ());

	menu.setLookAndFeel (&getLookAndFeel ());

	menu.showMenuAsync (juce::PopupMenu::Options ()
							.withTargetComponent (this)
							.withMinimumWidth (getWidth ())
							.withStandardItemHeight (juce::jmax (18, getHeight () - 6)),
						[safe = juce::Component::SafePointer<SyncedComboBox> (this)] (int result)
						{
							if (safe == nullptr)
							{
								return;
							}

							// the base class does this too: closing the menu has to release the
							// "a menu is open" state, or the box stops opening its popup
							safe->hidePopup ();

							if (result != 0)
							{
								safe->noteUserPick (result);
							}
						});
}

} // namespace toybox::ui
