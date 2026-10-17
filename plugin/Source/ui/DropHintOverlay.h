#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace toybox::ui
{

/**
	Full-size overlay that shows the "drop a file here" hint (a centred label plus a dashed
	outline). It is a component of its own, so showing and hiding it repaints exactly this
	area - the editor only has to call setVisible() from the drag & drop callbacks.

	It never takes mouse clicks: the editor underneath stays the drag & drop target.
*/
class DropHintOverlay : public juce::Component
{
public:
	DropHintOverlay (void);

	void paint (juce::Graphics& g) override;

	/** Re-applies the label's accent colour (see PluginEditor::lookAndFeelChanged). */
	void lookAndFeelChanged (void) override;

private:
	void resized (void) override;

	juce::Label label;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DropHintOverlay)
};

} // namespace toybox::ui
