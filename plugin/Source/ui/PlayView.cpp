#include "PlayView.h"

#include "SoftShadow.h"
#include "Theme.h"

#include "playback/PlaybackEngine.h"

using namespace toybox;
using namespace toybox::ui;

PlayView::PlayView (toybox::PlaybackEngine& playback, bool syncAvailable)
	: engine (playback),
	  songInfoPanel (playback),
	  transportPanel (playback, syncAvailable),
	  contentSlot (playback)
{
	addAndMakeVisible (songInfoPanel);
	addAndMakeVisible (transportPanel);
	// hidden until a file with sound chips is loaded (ChipPanel::rebuild sets the visibility)
	addChildComponent (chipPanel);
	addAndMakeVisible (contentSlot);

	contentSlot.onExportReport = [this] (const ExportReport& report)
	{
		if (onExportReport != nullptr)
		{
			onExportReport (report);
		}
	};
}

void PlayView::setContentView (ContentView view)
{
	contentSlot.setView (view);
	repaint ();
}

ContentView PlayView::getContentView (void) const
{
	return contentSlot.getView ();
}

void PlayView::paintCardShadows (juce::Graphics& g) const
{
	// The window paints these (see View::paintCardShadows): a shadow reaches outside its card, and
	// this page's own bounds end where the cards do. drawSoftShadow() skips the blur whenever the
	// repaint is entirely inside a shape (a moving slider thumb, a label that changed its text), so
	// the refresh repaints stay cheap.
	drawCardShadow (g, songInfoPanel.getBounds().toFloat());
	drawCardShadow (g, transportPanel.getBounds().toFloat());

	if (chipPanel.isVisible() && ! chipPanel.getBounds().isEmpty())
	{
		drawCardShadow (g, chipPanel.getBounds().toFloat());
	}

	if (! contentSlot.getBounds().isEmpty())
	{
		drawCardShadow (g, contentSlot.getBounds().toFloat());
	}
}

void PlayView::resized (void)
{
	auto area = getLocalBounds();

	// transport card (always at the bottom)
	const int transportHeight = juce::jmin (TransportPanel::preferredHeight,
											juce::jmax (90, area.getHeight() - ContentSlot::minHeight - 110));
	transportPanel.setBounds (area.removeFromBottom (transportHeight));
	area.removeFromBottom (theme::cardGap);

	// song info card
	const int infoHeight = juce::jmin (SongInfoPanel::preferredHeight,
									   juce::jmax (SongInfoPanel::minHeight, area.getHeight() - ContentSlot::minHeight));
	songInfoPanel.setBounds (area.removeFromTop (infoHeight));
	area.removeFromTop (theme::cardGap);

	// chip mute card - takes no space while hidden and never eats into the content area's minimum
	if (chipPanel.hasChips())
	{
		const int chipsHeight = juce::jmin (chipPanel.getPreferredHeight (area.getWidth()),
											juce::jmax (0, area.getHeight() - ContentSlot::minHeight));
		if (chipsHeight > 0)
		{
			chipPanel.setBounds (area.removeFromTop (chipsHeight));
			area.removeFromTop (theme::cardGap);
		}
		else
		{
			chipPanel.setBounds ({});
		}
	}
	else
	{
		chipPanel.setBounds ({});
	}

	// The content area owns everything that is left: the mixer, the chip state or the oscilloscope.
	contentSlot.setBounds (area);

	// The card shadows are painted by paint(), outside the card bounds - a relayout has to clear
	// the ring around a card that moved, appeared or changed size.
	repaint();
}

void PlayView::refresh (void)
{
	transportPanel.refresh();
	songInfoPanel.refresh();
	contentSlot.refresh();
}

void PlayView::refreshLanguage (void)
{
	transportPanel.refreshLanguage();
	songInfoPanel.refreshLanguage();
	chipPanel.refreshLanguage();
	contentSlot.refreshLanguage();
	repaint();
}

void PlayView::songChanged (void)
{
	rebuildChipButtons();		// relayouts as well: the chips card may appear or change height
	contentSlot.songChanged();	// the track list and the chip snapshot belong to the new song
	transportPanel.refresh();
	songInfoPanel.refresh();
	resized();
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
		if (transportPanel.isHostDriving())
		{
			return false;
		}

		transportPanel.togglePlayStop();
		return true;
	}
	if (key == juce::KeyPress::returnKey)
	{
		transportPanel.togglePlayPause();
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

void PlayView::rebuildChipButtons (void)
{
	// A backend that cannot silence a chip gets no card: the checkboxes would be decorative, and
	// the player would spend a session wondering why they do nothing.
	if (! engine.canMuteChips ())
	{
		chipPanel.rebuild ({}, [this] (size_t chipIndex, bool shouldBeMuted)
									{ engine.setChipMuted (chipIndex, shouldBeMuted); });
		resized ();
		return;
	}

	chipPanel.rebuild (engine.getChips(), [this] (size_t chipIndex, bool shouldBeMuted)
										   {
											   engine.setChipMuted (chipIndex, shouldBeMuted);
										   });
	resized();	// the card may appear/disappear or change its height
}
