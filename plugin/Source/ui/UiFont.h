#pragma once

#include "settings/SettingsStore.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace toybox::ui::fonts
{

/** The actual family the operating system uses for its UI (Segoe UI on Windows, San Francisco
	on macOS, the desktop's UI font on Linux).

	JUCE's `Font::getSystemUIFontName()` is only a placeholder that its typeface resolver
	understands; a LookAndFeel hands a name straight to the platform, so this helper asks for the
	resolved family once and answers with that instead. */
juce::String systemUiFamily (void);

/** Whether `family` is one of the installed typeface families, ignoring case. */
bool isInstalled (const juce::String& family);

/** The family to hand to JUCE for a stored choice: the custom family when it is still
	installed, otherwise the system UI family. */
juce::String resolvedFamily (settings::SettingsStore::FontMode mode, const juce::String& family);

/** Applies the stored choice to JUCE's default LookAndFeel.

	JUCE resolves every typeface through the *default* LookAndFeel, not through the one a
	component inherited, so this is where the choice has to go for labels, buttons and menus
	alike. It is called when a ToyboxLookAndFeel is built, before the editor creates any
	component that holds a Font; an already-built editor cannot be updated in place, which is
	why the Appearance page asks for the window to be reopened instead. */
void applyStoredSelection (void);

} // namespace toybox::ui::fonts
