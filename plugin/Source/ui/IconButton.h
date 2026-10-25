#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace toybox::ui
{

/** Small button that draws one of the Icons glyphs instead of a text label. */
class IconButton : public juce::Button
{
public:
	enum class Icon { play, pause, stop, loop, playStart, sliders, spreadOutputs, mergeToMaster,
					  sync, cue, dcBlock };

	/** `buttonId` is stable and independent of the language ("play", "spread-outputs"). The
		accessible name is translated by the owner and can change at runtime; deriving the ID from
		it would rename every button the first time the user switched to Chinese. */
	IconButton (const juce::String& buttonId, const juce::String& buttonName, Icon iconToDraw)
		: juce::Button (buttonName), icon (iconToDraw)
	{
		// The console harness finds controls by ID (docs/desgin/testing-architecture.md §3.1).
		setComponentID ("btn." + buttonId.toLowerCase().replaceCharacter (' ', '-'));
	}

	/** A word drawn under the glyph, inside the face ("SYNC", "CUE"). An empty caption leaves the
		glyph the whole face, which is what every other icon button in the editor wants; the two
		DAW-sync buttons take one because their glyphs alone do not say what they do. */
	void setCaption (const juce::String& text)
	{
		caption = text;
		repaint();
	}

	void paintButton (juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override;

	// the focus ring (ButtonPaint.h) is only correct if focus changes repaint the button
	void focusGained (juce::Component::FocusChangeType) override { repaint(); }
	void focusLost (juce::Component::FocusChangeType) override { repaint(); }

private:
	Icon icon;
	juce::String caption;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IconButton)
};

} // namespace toybox::ui
