#pragma once

#include <juce_core/juce_core.h>

#include <map>

namespace toybox::i18n
{

/**
	One translation file, parsed and validated.

	The on-disk format is deliberately small: metadata lines (`id:`, `language:`, `countries:`)
	and `"key" = "value";` entries, UTF-8. It is close enough to juce::LocalisedStrings that a
	translator who has seen JUCE's format will recognise it, but the parser is ours because the
	application needs two things JUCE does not expose: the language's own tag (for the fallback
	chain) and the line that made a file invalid (for the Language page).

	A pack is immutable after parsing. The manager swaps shared_ptrs rather than mutating one, so a
	window that is painting while a rescan happens can never see half a translation.
*/
class LanguagePack
{
public:
	LanguagePack (void) = default;

	/** Parses UTF-8 text. `sourceName` is only used in errors ("zh-Hans.lang line 12: ..."). */
	bool parse (const juce::String& text, const juce::String& sourceName, juce::String& error);

	/** Reads and parses a file. The file size is checked by the caller; this only reads it. */
	bool loadFromFile (const juce::File& file, juce::String& error);

	/** The language tag from `id:` - "en", "zh-Hans", "ja", ... Used for matching a user's
		translation of a built-in language and for following the system language on first run. */
	const juce::String& getLanguageTag (void) const { return languageTag; }

	/** The endonym from `language:` - "English", "简体中文". Never translated. */
	const juce::String& getDisplayName (void) const { return displayName; }

	/** Where it came from, for diagnostics. */
	const juce::String& getSourceName (void) const { return sourceName; }

	/** The translation for `key`, or an empty string when this pack does not have it. */
	juce::String find (const juce::String& key) const;

	bool contains (const juce::String& key) const;

	/** Keys the reference pack has and this one does not. Used by the settings page to tell a
		translator what their file is still missing; missing keys fall back at runtime. */
	int countMissing (const LanguagePack& reference) const;

	const std::map<juce::String, juce::String>& getEntries (void) const { return entries; }

private:
	juce::String languageTag;
	juce::String displayName;
	juce::String sourceName;
	std::map<juce::String, juce::String> entries;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LanguagePack)
};

} // namespace toybox::i18n
