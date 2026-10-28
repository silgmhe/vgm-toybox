#pragma once

#include "ChipPanel.h"
#include "ContentSlot.h"
#include "MixerPanel.h"
#include "SongInfoPanel.h"
#include "TransportPanel.h"
#include "View.h"

#include <functional>

#include "i18n/LanguageManager.h"

namespace toybox { class PlaybackEngine; }

namespace toybox::ui
{

/**
	The playback page: the song's context, one swappable content area, and the transport.

	What sits above the transport is chosen from the View menu - the mixer, the chip state or the
	oscilloscope - and it is the only thing that changes. The transport stays where it is because
	it is operated rather than read, and the song info and chip mutes stay because they belong to
	the song, not to whichever view is showing.
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

	/** Forwarded from the content slot: what the last export did.

		The window that reports it belongs to the editor rather than to this page - a report that
		dimmed only the keyboard card read as part of the card and left the rest of the window
		looking live while it was blocked.
	*/
	std::function<void (const ExportReport&)> onExportReport;

	/** The same shape for the two steps before it: this page asks the editor for the options
		window, and the editor calls beginExport() back with the answer. */
	std::function<void ()> onExportOptions;

	void beginExport (const ExportOptions& options);

	/** Which panel the content area is showing. The View menu and its Ctrl+1..3 map to this. */
	void setContentView (ContentView view);
	ContentView getContentView (void) const;

private:
	/** The chip checkboxes belong to the song, so this runs whenever there is a different one.
		It relayouts as well: the card can appear, disappear or change height. */
	void rebuildChipButtons (void);

	toybox::PlaybackEngine& engine;

	SongInfoPanel songInfoPanel;
	TransportPanel transportPanel;
	ChipPanel chipPanel;
	ContentSlot contentSlot;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlayView)
};

} // namespace toybox::ui
