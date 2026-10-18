#pragma once

#include "ToyboxLookAndFeel.h"
#include "settings/SettingsStore.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace toybox::ui
{

/**
	A small modal font picker: System UI, plus every installed family the font system reports.

	JUCE 8.0.9 has no native font dialog and no FontChooser class, so this is deliberately a
	component rather than a platform call. It lists the system fonts, filters them as the user
	types, shows a small and a normal-size preview, and hands the family back through onAccept.
	The dialog closes itself; the caller owns the result and decides when to apply it.
*/
class FontChooserDialog final : public juce::Component,
								private juce::ListBoxModel
{
public:
	FontChooserDialog (settings::SettingsStore::FontMode mode, const juce::String& family);

	/** Called with systemUI/empty for the first row, custom/family for an installed font. */
	std::function<void (settings::SettingsStore::FontMode, const juce::String&)> onAccept;

	void paint (juce::Graphics& g) override;
	void resized (void) override;
	void lookAndFeelChanged (void) override;
	bool keyPressed (const juce::KeyPress& key) override;

private:
	int getNumRows (void) override;
	void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool rowIsSelected) override;
	void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
	void selectedRowsChanged (int lastRowSelected) override;
	void returnKeyPressed (int lastRowSelected) override;

	void rebuild (void);
	void updatePreview (void);
	void accept (void);
	void cancel (void);
	void closeDialog (void);

	int rowForFamily (const juce::String& family) const;
	juce::String familyForRow (int row) const;

	// A dialog is a top-level window, so it does not inherit the editor's LookAndFeel; this
	// instance gives its buttons the same face and soft shadow as the rest of the window. It is
	// declared first so it is destroyed after the components that use it.
	ToyboxLookAndFeel lookAndFeel;

	// The model data has to outlive the ListBox that reads it through `this`: members are
	// destroyed in reverse order, so the rows are declared first.
	juce::StringArray allFonts;
	juce::StringArray filteredFonts;
	juce::String selectedFamily;

	juce::TextEditor search;
	juce::ListBox list;
	juce::Label previewLarge;
	juce::Label previewSmall;
	juce::TextButton okButton;
	juce::TextButton cancelButton;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FontChooserDialog)
};

} // namespace toybox::ui
