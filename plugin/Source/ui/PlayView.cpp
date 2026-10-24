#include "PlayView.h"

#include "SoftShadow.h"
#include "Theme.h"

using namespace toybox::ui;

PlayView::PlayView (toybox::PlaybackEngine& playback, bool syncAvailable)
	: transportPanel (playback, syncAvailable),
	  visualiserPanel (playback)
{
	addAndMakeVisible (visualiserPanel);
	addAndMakeVisible (transportPanel);
}

void PlayView::paintCardShadows (juce::Graphics& g) const
{
	// The window paints these (see View::paintCardShadows): a shadow reaches outside its card, and
	// this page's own bounds end where the cards do. drawSoftShadow() skips the blur whenever the
	// repaint is entirely inside a shape (a moving slider thumb, a label that changed its text), so
	// the refresh repaints stay cheap.
	drawCardShadow (g, visualiserPanel.getBounds ().toFloat ());
	drawCardShadow (g, transportPanel.getBounds ().toFloat ());
}

void PlayView::resized (void)
{
	auto area = getLocalBounds ();

	// The transport card is the part that is operated, so it is placed first and never squeezed
	// below its preferred height unless the window itself is too short.
	const int transportHeight = juce::jmin (TransportPanel::preferredHeight,
											juce::jmax (90, area.getHeight () - VisualiserPanel::minHeight - theme::cardGap));

	transportPanel.setBounds (area.removeFromBottom (transportHeight));
	area.removeFromBottom (theme::cardGap);

	visualiserPanel.setBounds (area);

	// The card shadows are painted outside the card bounds - a relayout has to clear the ring
	// around a card that moved or changed size.
	repaint ();
}

void PlayView::refresh (void)
{
	transportPanel.refresh ();
}

void PlayView::refreshLanguage (void)
{
	transportPanel.refreshLanguage ();
	visualiserPanel.refreshLanguage ();
	repaint ();
}

void PlayView::songChanged (void)
{
	visualiserPanel.songChanged ();
	transportPanel.refresh ();
	resized ();
}

bool PlayView::handleKey (const juce::KeyPress& key)
{
	if (key == juce::KeyPress::spaceKey)
	{
		// While the host drives the song the space bar is the host's. The local transport is
		// disabled then, so swallowing the key could only make it do nothing - the DAW's own
		// transport shortcut is what the user is after. Saying "not handled" is what lets it out:
		// the event leaves the editor and the wrapper passes it on (on Windows JUCE reposts an
		// unused WM_KEYDOWN to the parent window - juce_Windowing_windows.cpp), so the host runs
		// its play/pause. Nothing is unbound when sync goes off: this is a check, not a binding,
		// so the space bar is the local transport again the moment the host stops driving (see
		// TransportPanel::isHostDriving).
		if (transportPanel.isHostDriving ())
		{
			return false;
		}

		transportPanel.togglePlayStop ();
		return true;
	}
	if (key == juce::KeyPress::returnKey)
	{
		transportPanel.togglePlayPause ();
		return true;
	}
	if (key == juce::KeyPress::homeKey)
	{
		transportPanel.seekToSeconds (0.0);
		return true;
	}
	if (key == juce::KeyPress::leftKey)
	{
		transportPanel.seekRelative (-5.0);
		return true;
	}
	if (key == juce::KeyPress::rightKey)
	{
		transportPanel.seekRelative (5.0);
		return true;
	}

	return false;
}
