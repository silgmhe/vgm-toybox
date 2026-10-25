#include "UiFont.h"

namespace toybox::ui::fonts
{

juce::String systemUiFamily (void)
{
	// Resolving the system typeface is not free, and the UI font cannot change while the
	// process is alive, so ask once and keep the answer.
	static const auto family = []
	{
		if (auto typeface = juce::Typeface::findSystemTypeface())
		{
			const auto name = typeface->getName();

			// Only a name the font system can actually resolve is worth setting - the same check
			// the custom branch below does, and for the same reason. On a KDE desktop where
			// fc-match resolves sans-serif to a .ttc collection, the name this typeface reports
			// need not be one findAllTypefaceNames() lists, and a LookAndFeel told to use a family
			// it cannot find draws every glyph as nothing: the window comes up with its buttons,
			// cards and colours intact and not one character on it.
			for (const auto& installed : juce::Font::findAllTypefaceNames())
			{
				if (installed.equalsIgnoreCase (name))
				{
					return installed;
				}
			}
		}

		// Empty means "leave JUCE's own default alone". It used to return
		// Font::getSystemUIFontName() here, which is a placeholder this comment already admitted
		// a LookAndFeel may not understand - and setting it is exactly how the blank window
		// happened.
		return juce::String();
	}();

	return family;
}

bool isInstalled (const juce::String& family)
{
	const auto requested = family.trim();

	if (requested.isEmpty())
	{
		return false;
	}

	for (const auto& installed : juce::Font::findAllTypefaceNames())
	{
		if (installed.equalsIgnoreCase (requested))
		{
			return true;
		}
	}

	return false;
}

juce::String resolvedFamily (settings::SettingsStore::FontMode mode, const juce::String& family)
{
	if (mode == settings::SettingsStore::FontMode::custom)
	{
		const auto requested = family.trim();

		// Return the spelling the font system knows, not the one that happened to be stored:
		// a hand-edited file can differ in case and would otherwise miss.
		for (const auto& installed : juce::Font::findAllTypefaceNames())
		{
			if (installed.equalsIgnoreCase (requested))
			{
				return installed;
			}
		}
	}

	return systemUiFamily();
}

void applyStoredSelection (void)
{
	auto& store = settings::SettingsStore::getInstance();
	const auto family = resolvedFamily (store.getFontMode(), store.getFontFamily());

	// Empty keeps the name JUCE chose for itself. Setting an unresolvable one is worse than not
	// setting anything: the resolver finds no typeface and draws no glyphs, which is a window full
	// of empty controls rather than one in the wrong font.
	if (family.isNotEmpty())
	{
		juce::LookAndFeel::getDefaultLookAndFeel().setDefaultSansSerifTypefaceName (family);
	}
}

} // namespace toybox::ui::fonts
