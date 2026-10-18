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

			if (name.isNotEmpty())
			{
				return name;
			}
		}

		// A platform that cannot report its UI font still has JUCE's placeholder, which its own
		// resolver understands even if a LookAndFeel does not.
		return juce::Font::getSystemUIFontName();
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

	juce::LookAndFeel::getDefaultLookAndFeel().setDefaultSansSerifTypefaceName (family);
}

} // namespace toybox::ui::fonts
