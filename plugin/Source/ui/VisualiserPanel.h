#pragma once

#include "playback/ChipState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace toybox { class PlaybackEngine; }

namespace toybox::ui
{

/**
	The chip visualisation, as two tabs, sitting where the mixer used to be.

	"The chip" shows what each channel is doing: its note lit on a keyboard, its level, and - for
	the chips that read one - the waveform it is playing. "The oscilloscope" is a placeholder.

	Both read the same snapshot the backend fills in, copied under the engine's own lock rather
	than read out of the audio thread's registers, and the capture stops whenever this panel is
	not on screen.
*/
class VisualiserPanel : public juce::Component,
						private juce::Timer
{
public:
	explicit VisualiserPanel (toybox::PlaybackEngine& engineToUse);
	~VisualiserPanel (void) override;

	void paint (juce::Graphics& g) override;
	void resized (void) override;

	void refreshLanguage (void);
	void songChanged (void);

	/** Card metrics, so the page can lay the transport out without guessing. */
	static constexpr int minHeight = 220;

private:
	class ChipPanel;
	class ScopePanel;

	void timerCallback (void) override;
	void exportWaves (void);

	toybox::PlaybackEngine& engine;
	playback::ChipStateSnapshot chipState;
	int lastChannelCount = -1;

	juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
	std::unique_ptr<ChipPanel> chipPanel;
	std::unique_ptr<ScopePanel> scopePanel;

	std::unique_ptr<juce::FileChooser> waveChooser;
	juce::String exportStatus;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VisualiserPanel)
};

} // namespace toybox::ui
