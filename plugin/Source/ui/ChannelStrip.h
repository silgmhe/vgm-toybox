#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "playback/OutputRouting.h"

namespace toybox::ui
{

/**
	One mixer channel strip in the usual DAW layout: the track name on top, the pan knob below
	it, a vertical level meter with its dB graduations next to the vertical gain fader and - on
	track strips - the mute (red) and solo (amber) buttons at the bottom. The master strip
	reuses the same class with the knob and the buttons hidden.

	Both the meter height and the fader travel follow the dB scale of `MeterScale.h`, so the cap
	lines up with the tick marks and with the level the channel produces.

	The strip never reads the engine - MixerPanel feeds it through setValues() and forwards the
	user's changes through the callbacks, so it does not know where the values come from.
*/
class ChannelStrip : public juce::Component
{
public:
	/** Height the strip lays itself out for (MixerPanel uses it for its own layout). */
	static constexpr int preferredHeight = 196;

	/** One entry of the output selector: the internal master bus or a real output bus. */
	struct OutputOption
	{
		playback::OutputTarget target = playback::kMasterOutput;
		juce::String name;
		bool connected = true;
	};

	ChannelStrip (void);

	/** Names the strip and picks the layout (track strip or master strip). */
	void setup (const juce::String& stripName, bool isMasterStrip);

	/** Updates every control without firing the callbacks; the meters are already decayed. */
	void setValues (float gain, float pan, bool muted, bool soloed, float peakL, float peakR, float dim);

	/** Fills the output selector - the panel builds the list, the strip only shows it. */
	void setOutputOptions (const std::vector<OutputOption>& options, playback::OutputTarget selected);

	/** Greys out the selector while the mixer is off (the routing is inactive then). */
	void setMixerActive (bool active);

	/** The target the selector currently shows - the panel uses it to avoid needless rebuilds. */
	playback::OutputTarget getShownOutput (void) const { return shownOutput; }

	std::function<void (float)> onGainChanged;
	std::function<void (float)> onPanChanged;
	std::function<void (bool)> onMuteChanged;
	std::function<void (bool)> onSoloChanged;
	std::function<void (playback::OutputTarget)> onOutputChanged;

	void paint (juce::Graphics& g) override;
	void resized (void) override;

private:
	void drawMeter (juce::Graphics& g) const;
	void drawScale (juce::Graphics& g) const;

	juce::TextButton muteButton { "M" };
	juce::TextButton soloButton { "S" };
	juce::ComboBox outputBox;		// Master / Out 1/2 / Out 3/4 ... at the bottom of the strip
	juce::Slider gainSlider { juce::Slider::LinearVertical, juce::Slider::NoTextBox };
	juce::Slider panKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };

	std::vector<OutputOption> outputOptions;	// parallel to the combo box's item ids
	// -2 is never a valid target (the engine clamps everything else to the master bus), so it
	// tells "the selector has not been filled yet" apart from "it shows the master bus"
	playback::OutputTarget shownOutput = -2;
	juce::String stripName;
	juce::Rectangle<int> nameBounds;
	juce::Rectangle<int> meterBounds;
	juce::Rectangle<int> scaleBounds;
	bool master = false;
	float peakL = 0.0f;
	float peakR = 0.0f;
	// what the meter area currently shows - the 20 Hz refresh only repaints when this changes
	float drawnPeakL = -1.0f;
	float drawnPeakR = -1.0f;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
};

} // namespace toybox::ui
