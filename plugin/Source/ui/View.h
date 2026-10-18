#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace toybox::ui
{

/**
	One page of the window.

	The window is a thin shell around these: the menu bar, the open button and the status line, and
	then - filling everything below - a ViewStack holding one View at a time. A view owns its own
	controls, lays them out itself and pulls its own state from the engine, which is what lets the
	playback page and a page that visualises the song sit next to each other without either of them
	knowing the other exists.

	Nothing here knows about songs or playback: a view is told when the song changed and when it
	should refresh, and decides what to do about it. Adding a page means writing one of these and
	handing it to the stack - the View menu is built from the stack, so no menu code changes either.
*/
class View : public juce::Component
{
public:
	~View (void) override = default;

	/** The name the View menu shows for this page. */
	virtual juce::String getViewName (void) const = 0;

	/** Called by the window's refresh timer while this view is the visible one. Keep it cheap: it
		runs 10-15 times a second, and the controls inside decide what actually repaints. */
	virtual void refresh (void) {}

	/** A song was loaded, closed or replaced. Sent to every view, visible or not, so a hidden
		page is already current when it is switched to. */
	virtual void songChanged (void) {}

	/** A key no focused control wanted. Returning true consumes it - that is how the transport
		keys keep working from any page while a slider or a button happens to have focus. */
	virtual bool handleKey (const juce::KeyPress&) { return false; }

	/** Draws the shadows of the cards this page put down, in the page's own coordinates.

		The page decides where they are; the window paints them, because a shadow reaches outside
		the card it belongs to and this component's bounds end where the cards do (SoftShadow.h has
		the general rule). Painting them here would cut every shadow off along the cards' edges -
		a straight line, most visible at the bottom corners. */
	virtual void paintCardShadows (juce::Graphics&) const {}
};

} // namespace toybox::ui
