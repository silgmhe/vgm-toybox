#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

namespace vgmplay::ui
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
	static constexpr int preferredHeight = 170;

	ChannelStrip (void);

	/** Names the strip and picks the layout (track strip or master strip). */
	void setup (const juce::String& stripName, bool isMasterStrip);

	/** Updates every control without firing the callbacks; the meters are already decayed. */
	void setValues (float gain, float pan, bool muted, bool soloed, float peakL, float peakR, float dim);

	std::function<void (float)> onGainChanged;
	std::function<void (float)> onPanChanged;
	std::function<void (bool)> onMuteChanged;
	std::function<void (bool)> onSoloChanged;

	void paint (juce::Graphics& g) override;
	void resized (void) override;

private:
	void drawMeter (juce::Graphics& g) const;
	void drawScale (juce::Graphics& g) const;

	juce::TextButton muteButton { "M" };
	juce::TextButton soloButton { "S" };
	juce::Slider gainSlider { juce::Slider::LinearVertical, juce::Slider::NoTextBox };
	juce::Slider panKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };

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

} // namespace vgmplay::ui
