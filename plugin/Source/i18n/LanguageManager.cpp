#include "LanguageManager.h"

#include "I18nData.h"
#include "settings/SettingsStore.h"

#include <algorithm>
#include <utility>

namespace toybox::i18n
{

namespace
{
	constexpr int kMaxExternalFiles = 128;
	constexpr juce::int64 kMaxExternalFileSize = 1024 * 1024;

	juce::String builtInSourceName (const juce::String& tag)
	{
		return "built-in " + tag;
	}
}

LanguageManager& LanguageManager::getInstance (void)
{
	static LanguageManager instance;
	return instance;
}

juce::File LanguageManager::externalDirectory (void)
{
	return toybox::settings::SettingsStore::settingsDirectory().getChildFile ("i18n");
}

void LanguageManager::initialise (void)
{
	if (initialised)
	{
		return;
	}

	initialised = true;
	warnings.clear();
	builtInWarnings.clear();
	entries.clear();
	current.reset();
	english.reset();

	std::vector<Entry> builtIns;
	loadBuiltIn ("en_lang", "en", builtIns);
	// juce_add_binary_data mangles "zh-Hans.lang" to "zhHans_lang" (the hyphen is dropped, not
	// turned into an underscore) - the name here is the generated one, not the file's.
	loadBuiltIn ("zhHans_lang", "zh-Hans", builtIns);

	entries = std::move (builtIns);

	for (const auto& entry : entries)
	{
		if (entry.info.languageTag.equalsIgnoreCase ("en"))
		{
			english = entry.pack;
			break;
		}
	}

	scanExternal();
	applyStoredSelection();
}

bool LanguageManager::loadBuiltIn (const char* resourceName, const juce::String& tag,
								   std::vector<Entry>& destination)
{
	int size = 0;
	const auto* data = I18nData::getNamedResource (resourceName, size);

	if (data == nullptr || size <= 0)
	{
		const auto message = "Built-in language resource is missing: " + juce::String (resourceName);
		warnings.add (message);
		builtInWarnings.add (message);
		return false;
	}

	auto pack = std::make_shared<LanguagePack>();
	juce::String error;

	if (! pack->parse (juce::String::fromUTF8 (data, size), builtInSourceName (tag), error))
	{
		warnings.add (error);
		builtInWarnings.add (error);
		return false;
	}

	Entry entry;
	entry.info.id = "builtin:" + pack->getLanguageTag();
	entry.info.languageTag = pack->getLanguageTag();
	entry.info.name = pack->getDisplayName();
	entry.info.builtIn = true;
	entry.pack = std::move (pack);
	destination.push_back (std::move (entry));
	return true;
}

void LanguageManager::scanExternal (void)
{
	auto directory = externalDirectory();

	if (! directory.isDirectory() && ! directory.createDirectory())
	{
		warnings.add ("Could not create the language folder: " + directory.getFullPathName());
		return;
	}

	std::vector<juce::File> files;
	juce::Array<juce::File> found;
	directory.findChildFiles (found, juce::File::findFiles, false, "*.lang");

	for (const auto& file : found)
	{
		const auto name = file.getFileName();

		// A leading dot is a hidden file; an underscore is the conventional "not loaded" prefix
		// for a template. Neither should appear in the user's language list.
		if (name.startsWithChar ('.') || name.startsWithChar ('_'))
		{
			continue;
		}

		files.push_back (file);
	}

	std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
	{
		return a.getFileName().compareIgnoreCase (b.getFileName()) < 0;
	});

	if ((int) files.size() > kMaxExternalFiles)
	{
		warnings.add ("Only the first " + juce::String (kMaxExternalFiles)
					  + " language files in " + directory.getFullPathName() + " are loaded");
		files.resize ((size_t) kMaxExternalFiles);
	}

	for (const auto& file : files)
	{
		if (file.getSize() > kMaxExternalFileSize)
		{
			warnings.add (file.getFileName() + ": larger than 1 MiB, skipped");
			continue;
		}

		auto pack = std::make_shared<LanguagePack>();
		juce::String error;

		if (! pack->loadFromFile (file, error))
		{
			warnings.add (error);
			continue;
		}

		Entry entry;
		entry.info.id = "external:" + file.getFileName();
		entry.info.languageTag = pack->getLanguageTag();
		entry.info.name = pack->getDisplayName();
		entry.info.file = file;
		entry.info.builtIn = false;
		entry.pack = std::move (pack);
		entries.push_back (std::move (entry));
	}
}

bool LanguageManager::setCurrent (const Entry* entry)
{
	auto next = entry != nullptr ? entry->pack : nullptr;

	if (current == next)
	{
		return false;
	}

	current = std::move (next);
	return true;
}

const LanguageManager::Entry* LanguageManager::findEntry (const juce::String& id) const
{
	for (const auto& entry : entries)
	{
		if (entry.info.id == id)
		{
			return &entry;
		}
	}

	return nullptr;
}

const LanguageManager::Entry* LanguageManager::findBuiltIn (const juce::String& tag) const
{
	for (const auto& entry : entries)
	{
		if (entry.info.builtIn && entry.info.languageTag.equalsIgnoreCase (tag))
		{
			return &entry;
		}
	}

	return nullptr;
}

void LanguageManager::chooseDefault (void)
{
	auto tag = juce::SystemStats::getUserLanguage();

	// Only two built-in languages exist today. The mapping is kept explicit so adding a third
	// built-in does not change what an English or Chinese desktop opens with.
	if (! tag.equalsIgnoreCase ("zh"))
	{
		tag = "en";
	}
	else
	{
		tag = "zh-Hans";
	}

	if (const auto* entry = findBuiltIn (tag))
	{
		setCurrent (entry);
		return;
	}

	if (const auto* entry = findBuiltIn ("en"))
	{
		setCurrent (entry);
	}
}

bool LanguageManager::applyStoredSelection (void)
{
	initialise();

	const auto previous = current;
	const auto storedId = toybox::settings::SettingsStore::getInstance().getLanguageId();
	const auto storedTag = toybox::settings::SettingsStore::getInstance().getLanguageTag();

	requestedId = storedId;
	selectionWarning.clear();

	if (storedId.isNotEmpty())
	{
		if (const auto* entry = findEntry (storedId))
		{
			setCurrent (entry);
			return previous != current;
		}

		// The selection may be a file the user deleted or renamed, or a built-in pack from a
		// future version. Use the language tag stored with it before falling back to the system
		// language; keep the missing id in the store so it comes back if the file returns.
		if (const auto* fallback = findBuiltIn (storedTag))
		{
			setCurrent (fallback);
		}
		else
		{
			chooseDefault();
		}

		selectionWarning = translate ("language.missingFile", storedId);
		return previous != current;
	}

	chooseDefault();
	return previous != current;
}

bool LanguageManager::rescan (void)
{
	initialise();

	// Built-in packs are immutable and stay at the front. Only the external tail is replaced.
	entries.erase (std::remove_if (entries.begin(), entries.end(), [] (const Entry& entry)
	{
		return ! entry.info.builtIn;
	}), entries.end());

	warnings = builtInWarnings;

	// Rebuild the external warnings only: a rescan is also what the Language page asks for after
	// a user fixes a file, and keeping stale errors next to the new list would be misleading.
	// Built-in errors, on the other hand, cannot change without rebuilding the application.
	if (english == nullptr)
	{
		for (const auto& entry : entries)
		{
			if (entry.info.languageTag.equalsIgnoreCase ("en"))
			{
				english = entry.pack;
				break;
			}
		}
	}

	scanExternal();
	return applyStoredSelection();
}

bool LanguageManager::selectById (const juce::String& id)
{
	initialise();

	const auto* entry = findEntry (id);

	if (entry == nullptr)
	{
		return false;
	}

	requestedId = id;
	selectionWarning.clear();
	setCurrent (entry);

	toybox::settings::SettingsStore::getInstance().setLanguageSelection (id, entry->info.languageTag);
	return true;
}

std::vector<LanguageInfo> LanguageManager::getAvailable (void)
{
	initialise();

	std::vector<LanguageInfo> infos;
	infos.reserve (entries.size());

	for (const auto& entry : entries)
	{
		infos.push_back (entry.info);
	}

	return infos;
}

juce::String LanguageManager::getCurrentId (void)
{
	initialise();

	for (const auto& entry : entries)
	{
		if (entry.pack == current)
		{
			return entry.info.id;
		}
	}

	return {};
}

juce::String LanguageManager::getCurrentLanguageTag (void)
{
	initialise();
	return current != nullptr ? current->getLanguageTag() : juce::String ("en");
}

juce::String LanguageManager::getCurrentName (void)
{
	initialise();
	return current != nullptr ? current->getDisplayName() : juce::String ("English");
}

juce::String LanguageManager::getDisplayName (const LanguageInfo& info)
{
	initialise();

	const auto qualifier = info.builtIn
		? translate ("language.builtinSuffix")
		: info.file.getFileNameWithoutExtension();

	return translate ("language.displayName", info.name, qualifier);
}

int LanguageManager::getCurrentMissingKeyCount (void)
{
	initialise();
	return current != nullptr && english != nullptr ? current->countMissing (*english) : 0;
}

juce::String LanguageManager::translate (const juce::String& key)
{
	initialise();

	if (key.isEmpty())
	{
		return {};
	}

	if (current != nullptr)
	{
		const auto value = current->find (key);
		if (value.isNotEmpty())
		{
			return value;
		}
	}

	// A user file for Chinese falls back to the built-in Chinese before English. The current pack
	// may itself be a built-in; find() above already searched it, so this only finds a base pack
	// when the current one is external.
	if (current != nullptr && ! current->getLanguageTag().equalsIgnoreCase ("en"))
	{
		if (const auto* base = findBuiltIn (current->getLanguageTag()))
		{
			const auto value = base->pack->find (key);
			if (value.isNotEmpty())
			{
				return value;
			}
		}
	}

	if (english != nullptr && current != english)
	{
		const auto value = english->find (key);
		if (value.isNotEmpty())
		{
			return value;
		}
	}

	return key;
}

juce::String LanguageManager::translate (const juce::String& key, const juce::String& arg0)
{
	return translate (key).replace ("{0}", arg0);
}

juce::String LanguageManager::translate (const juce::String& key, const juce::String& arg0,
										const juce::String& arg1)
{
	return translate (key, arg0).replace ("{1}", arg1);
}

juce::String tr (const juce::String& key)
{
	return LanguageManager::getInstance().translate (key);
}

juce::String tr (const juce::String& key, const juce::String& arg0)
{
	return LanguageManager::getInstance().translate (key, arg0);
}

juce::String tr (const juce::String& key, const juce::String& arg0, const juce::String& arg1)
{
	return LanguageManager::getInstance().translate (key, arg0, arg1);
}

} // namespace toybox::i18n
