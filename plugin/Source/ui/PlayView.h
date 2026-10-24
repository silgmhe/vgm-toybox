#pragma once

#include "TransportPanel.h"
#include "View.h"
#include "VisualiserPanel.h"

#include "i18n/LanguageManager.h"

namespace toybox { class PlaybackEngine; }

namespace toybox::ui
{

/**
	The playback page: the chip visualisation above the transport.

	It used to hold the song info card, the chip mutes and the mixer. Those are gone - the mixer
	and the info card by choice, so the window shows one thing at a time - and what sits above the
	transport is now the visualisation, as two tabs.

	The transport stays where it was, at the bottom, always visible: it is the part of the page
	that is operated rather than read.
*/
class PlayView : public View
{
public:
	/** `syncAvailable` is forwarded to the transport card: only a hosted build has a play head to
		follow (see TransportPanel). */
	explicit PlayView (toybox::PlaybackEngine& engine, bool syncAvailable = true);

	juce::String getViewId (void) const override { return "play"; }
	juce::String getViewName (void) const override { return toybox::i18n::tr ("view.play"); }

	void paintCardShadows (juce::Graphics& g) const override;
	void resized (void) override;
	void refresh (void) override;
	void refreshLanguage (void) override;
	void songChanged (void) override;
	bool handleKey (const juce::KeyPress& key) override;

private:
	TransportPanel transportPanel;
	VisualiserPanel visualiserPanel;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlayView)
};

} // namespace toybox::ui
