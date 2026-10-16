#include "PlayView.h"

#include "SoftShadow.h"
#include "Theme.h"

#include "playback/PlaybackEngine.h"

using namespace toybox;
using namespace toybox::ui;

PlayView::PlayView (toybox::PlaybackEngine& playback)
	: engine (playback),
	  songInfoPanel (playback),
	  transportPanel (playback),
	  mixerPanel (playback)
{
	addAndMakeVisible (songInfoPanel);
	addAndMakeVisible (transportPanel);
	// hidden until a file with sound chips is loaded (ChipPanel::rebuild sets the visibility)
	addChildComponent (chipPanel);
	// hidden while no song with tracks is loaded (MixerPanel::refresh sets the visibility)
	addChildComponent (mixerPanel);
}

void PlayView::paint (juce::Graphics& g)
{
	// The cards are clipped to their own bounds, so the soft shadows they cast onto the page have
	// to be painted here, before them. drawSoftShadow() skips the blur whenever the repaint is
	// entirely inside a shape (a moving slider thumb, a label that changed its text), so the
	// refresh repaints stay cheap.
	drawCardShadow (g, songInfoPanel.getBounds().toFloat());
	drawCardShadow (g, transportPanel.getBounds().toFloat());

	if (chipPanel.isVisible() && ! chipPanel.getBounds().isEmpty())
	{
		drawCardShadow (g, chipPanel.getBounds().toFloat());
	}
	if (mixerPanel.isVisible() && ! mixerPanel.getBounds().isEmpty())
	{
		drawCardShadow (g, mixerPanel.getBounds().toFloat());
	}
}

void PlayView::resized (void)
{
	auto area = getLocalBounds();

	// transport card (always at the bottom)
	const int transportHeight = juce::jmin (TransportPanel::preferredHeight, juce::jmax (90, area.getHeight() - 110));
	transportPanel.setBounds (area.removeFromBottom (transportHeight));
	area.removeFromBottom (theme::cardGap);

	// song info card
	const int infoHeight = juce::jmin (SongInfoPanel::preferredHeight,
									   juce::jmax (SongInfoPanel::minHeight, area.getHeight() - 80));
	songInfoPanel.setBounds (area.removeFromTop (infoHeight));
	area.removeFromTop (theme::cardGap);

	// chip mute card (+ section header) - it takes no space while it is hidden and never eats
	// into the mixer's minimum height
	if (chipPanel.hasChips())
	{
		const int chipsHeight = juce::jmin (chipPanel.getPreferredHeight (area.getWidth()),
											juce::jmax (0, area.getHeight() - MixerPanel::minimumHeight));
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

	// The mixer card owns the whole area between the chips card and the transport card - it
	// simply follows the window height (its strips scroll inside).
	if (mixerPanel.isVisible())
	{
		mixerPanel.setBounds (area);
	}
	else
	{
		mixerPanel.setBounds ({});
	}

	// The card shadows are painted by paint(), outside the card bounds - a relayout has to clear
	// the ring around a card that moved, appeared or changed size.
	repaint();
}

void PlayView::refresh (void)
{
	transportPanel.refresh();
	songInfoPanel.refresh();
	mixerPanel.refresh();
}

void PlayView::songChanged (void)
{
	rebuildChipButtons();		// relayouts as well: the chips card may appear or change height
	mixerPanel.refresh();		// the track list belongs to the new song (may show/hide the card)
	transportPanel.refresh();
	songInfoPanel.refresh();
	resized();
}

bool PlayView::handleKey (const juce::KeyPress& key)
{
	if (key == juce::KeyPress::spaceKey)
	{
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
	chipPanel.rebuild (engine.getChips(), [this] (size_t chipIndex, bool shouldBeMuted)
										   {
											   engine.setChipMuted (chipIndex, shouldBeMuted);
										   });
	resized();	// the card may appear/disappear or change its height
}
