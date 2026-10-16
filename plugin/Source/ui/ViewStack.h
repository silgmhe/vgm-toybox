#pragma once

#include "View.h"

namespace toybox::ui
{

/**
	Holds the window's pages and shows exactly one of them.

	The stack owns its views and hands the visible one the whole area it was given - a view lays
	itself out inside that. Switching pages, refreshing them and routing the keys nobody else
	wanted is all it does, and the View menu in ui/MainMenu.h is built from `getViewNames()`, so a
	new page shows up there without any menu code being touched.

	The first view added becomes the visible one. Views that are not on screen keep their bounds and
	their state; they are still told about `songChanged()`, which is what keeps a page correct
	after a song was loaded while it was hidden.
*/
class ViewStack : public juce::Component
{
public:
	// the copy macro below suppresses the implicit default constructor (same reason ChipPanel has
	// this line)
	ViewStack (void) = default;

	/** Takes ownership. */
	void addView (View* view)
	{
		jassert (view != nullptr);

		views.add (view);
		addChildComponent (view);		// hidden until it is the current one

		if (current < 0)
		{
			setCurrentView (0);
		}
	}

	int getNumViews (void) const { return views.size(); }
	View* getView (int index) const { return views[index]; }
	int getCurrentViewIndex (void) const { return current; }
	View* getCurrentView (void) const { return views[current]; }

	/** The page names, in menu order - what ui/MainMenu.h builds its View menu from. */
	juce::StringArray getViewNames (void) const
	{
		juce::StringArray names;
		for (const auto* view : views)
		{
			names.add (view->getViewName());
		}
		return names;
	}

	void setCurrentView (int index)
	{
		if (! juce::isPositiveAndBelow (index, views.size()) || index == current)
		{
			return;
		}

		if (auto* previous = getCurrentView())
		{
			previous->setVisible (false);
		}

		current = index;

		auto* view = views[current];
		view->setBounds (getLocalBounds());
		view->setVisible (true);
		view->refresh();

		if (onViewChanged != nullptr)
		{
			onViewChanged (current);
		}
	}

	/** Called when the page changed - the window keeps the menu's tick in step with this. */
	std::function<void (int)> onViewChanged;

	void refresh (void)
	{
		if (auto* view = getCurrentView())
		{
			view->refresh();
		}
	}

	void songChanged (void)
	{
		for (auto* view : views)
		{
			view->songChanged();
		}
	}

	bool handleKey (const juce::KeyPress& key)
	{
		return getCurrentView() != nullptr && getCurrentView()->handleKey (key);
	}

private:
	void resized (void) override
	{
		if (auto* view = getCurrentView())
		{
			view->setBounds (getLocalBounds());
		}
	}

	juce::OwnedArray<View> views;
	int current = -1;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ViewStack)
};

} // namespace toybox::ui
