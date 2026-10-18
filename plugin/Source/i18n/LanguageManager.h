#pragma once

#include "LanguagePack.h"

#include <memory>
#include <vector>

namespace toybox::i18n
{

/** One entry in the Language page's list. */
struct LanguageInfo
{
	juce::String id;			// "builtin:zh-Hans" or "external:my-chinese.lang"
	juce::String languageTag;	// the `id:` inside the file ("zh-Hans")
	juce::String name;			// the `language:` inside the file, in its own language
	juce::File file;			// empty for built-in packs
	bool builtIn = false;
};

/**
	Finds, loads and switches between the built-in translations and the user's files.

	The external folder is `<SettingsStore::settingsDirectory()>/i18n`; the same folder the
	settings file lives in, so a portable install and the tests' temporary directory work without
	a second path convention. Built-in packs are compiled in as BinaryData: a plugin bundle cannot
	rely on a neighbouring resources folder, and the standalone and VST3 must show the same list.

	The manager owns no UI. A change is stored through SettingsStore and broadcast there; every
	open editor listens to that one store, which is also what keeps the plugin and standalone in
	step. `translate()` never touches a global juce::LocalisedStrings instance, so a tooltip built
	while another component is switching languages cannot see a half-installed language.

	Fallback is deliberately explicit:

	    selected file -> built-in pack with the same language tag -> English -> the key itself

	An external file may therefore translate only a handful of strings; the rest come from the
	built-in pack for its declared language, then English. A file whose selected name has
	disappeared falls back the same way and leaves a warning on the Language page rather than
	quietly rewriting the stored selection.
*/
class LanguageManager
{
public:
	static LanguageManager& getInstance (void);

	/** Loads the built-in packs and scans the external folder. Idempotent. */
	void initialise (void);

	/** Rebuilds the external list and re-reads the stored selection. Returns true when the pack
		actually in force changed (a rescan that only adds files does not refresh every window). */
	bool rescan (void);

	/** Reads `ui.language` / `ui.languageTag` from the store and puts that pack in force without
		writing anything back. Called on startup, and when the settings file is reset/reloaded. */
	bool applyStoredSelection (void);

	/** Selects a listed pack, writes both the pack id and its language tag, and lets the settings
		store broadcast the change. False with no change when the id is not in the current list. */
	bool selectById (const juce::String& id);

	std::vector<LanguageInfo> getAvailable (void);

	juce::String getCurrentId (void);
	juce::String getCurrentLanguageTag (void);
	juce::String getCurrentName (void);

	/** `简体中文（内置）`, `简体中文 (my-file)`, ... The name itself is never translated. */
	juce::String getDisplayName (const LanguageInfo& info);

	/** The name of the pack that was requested (which may be a file that has since gone). */
	juce::String getRequestedId (void) const { return requestedId; }

	/** Empty, or one line explaining why the requested pack could not be used. */
	juce::String getSelectionWarning (void) const { return selectionWarning; }

	/** Parse errors, oversized files, unreadable folders, ... One line each. */
	const juce::StringArray& getWarnings (void) const { return warnings; }

	/** How many keys of the English pack the current pack lacks. */
	int getCurrentMissingKeyCount (void);

	/** The translation for `key`, with the fallback chain above. Never returns an empty string
		unless the key itself is empty. */
	juce::String translate (const juce::String& key);

	juce::String translate (const juce::String& key, const juce::String& arg0);
	juce::String translate (const juce::String& key, const juce::String& arg0,
							const juce::String& arg1);

	static juce::File externalDirectory (void);

private:
	LanguageManager (void) = default;

	struct Entry
	{
		LanguageInfo info;
		std::shared_ptr<LanguagePack> pack;
	};

	bool loadBuiltIn (const char* resourceName, const juce::String& tag,
					  std::vector<Entry>& destination);
	void scanExternal (void);
	bool setCurrent (const Entry* entry);
	const Entry* findEntry (const juce::String& id) const;
	const Entry* findBuiltIn (const juce::String& tag) const;
	void chooseDefault (void);

	std::vector<Entry> entries;
	std::shared_ptr<LanguagePack> current;
	std::shared_ptr<LanguagePack> english;
	juce::String requestedId;
	juce::String selectionWarning;
	juce::StringArray warnings;
	juce::StringArray builtInWarnings;
	bool initialised = false;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LanguageManager)
};

/** The short form used by the UI: `i18n::tr("menu.file.open")`. */
juce::String tr (const juce::String& key);
juce::String tr (const juce::String& key, const juce::String& arg0);
juce::String tr (const juce::String& key, const juce::String& arg0, const juce::String& arg1);

} // namespace toybox::i18n
