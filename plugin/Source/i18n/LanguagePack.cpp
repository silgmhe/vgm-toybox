#include "LanguagePack.h"

namespace toybox::i18n
{

namespace
{
	/** Reads one quoted or bare token. Bare values are accepted so a hand-written `id: zh-Hans`
		works; quoted values are the documented form because they can contain spaces and `=`. */
	bool readToken (const juce::String& text, juce::String& out, juce::String& error)
	{
		auto value = text.trim();

		if (! value.startsWithChar ('"'))
		{
			if (value.endsWithChar (';'))
			{
				value = value.dropLastCharacters (1).trimEnd();
			}

			out = value;
			return true;
		}

		juce::String result;
		bool closed = false;

		for (int i = 1; i < value.length(); ++i)
		{
			const auto character = value[i];

			if (character == '\\' && i + 1 < value.length())
			{
				const auto escaped = value[++i];

				switch (escaped)
				{
					case 'n':	result << juce::newLine;	break;
					case 'r':	result << '\r';			break;
					case 't':	result << '\t';			break;
					case '"':	result << '"';			break;
					case '\\':	result << '\\';			break;
					default:	result << escaped;		break;
				}

				continue;
			}

			if (character == '"')
			{
				closed = true;

				// Only a statement terminator and whitespace may follow the closing quote.
				const auto rest = value.substring (i + 1).trim();
				if (rest.isNotEmpty() && rest != ";")
				{
					error = "unexpected text after closing quote";
					return false;
				}

				break;
			}

			result << character;
		}

		if (! closed)
		{
			error = "unterminated quoted value";
			return false;
		}

		out = result;
		return true;
	}

	int findUnquotedEquals (const juce::String& line)
	{
		bool quoted = false;

		for (int i = 0; i < line.length(); ++i)
		{
			const auto character = line[i];

			if (character == '\\' && quoted && i + 1 < line.length())
			{
				++i;
				continue;
			}

			if (character == '"')
			{
				quoted = ! quoted;
			}
			else if (character == '=' && ! quoted)
			{
				return i;
			}
		}

		return -1;
	}
}

bool LanguagePack::parse (const juce::String& text, const juce::String& name, juce::String& error)
{
	sourceName = name;
	languageTag.clear();
	displayName.clear();
	entries.clear();

	auto content = text;

	// A UTF-8 BOM is invisible in an editor and would otherwise be read as part of the first
	// metadata name. stripBom is not enough here because a BOM may precede a comment line.
	if (content.startsWithChar (juce::juce_wchar (0xfeff)))
	{
		content = content.substring (1);
	}

	const auto lines = juce::StringArray::fromLines (content);

	for (int i = 0; i < lines.size(); ++i)
	{
		auto line = lines[i].trim();

		if (line.isEmpty() || line.startsWith ("//") || line.startsWith ("#"))
		{
			continue;
		}

		const auto colon = line.indexOfChar (':');
		const auto metadataName = colon > 0 ? line.substring (0, colon).trim() : juce::String();

		if (metadataName.equalsIgnoreCase ("id")
			|| metadataName.equalsIgnoreCase ("language")
			|| metadataName.equalsIgnoreCase ("countries")
			|| metadataName.equalsIgnoreCase ("format"))
		{
			juce::String metadataValue;
			juce::String tokenError;

			if (! readToken (line.substring (colon + 1), metadataValue, tokenError))
			{
				error = sourceName + " line " + juce::String (i + 1) + ": " + tokenError;
				return false;
			}

			if (metadataName.equalsIgnoreCase ("id"))
			{
				languageTag = metadataValue;
			}
			else if (metadataName.equalsIgnoreCase ("language"))
			{
				displayName = metadataValue;
			}

			// `countries:` and `format:` are accepted for LocalisedStrings compatibility and
			// future format changes; neither changes how a file is read today.

			continue;
		}

		const auto equals = findUnquotedEquals (line);

		if (equals < 0)
		{
			error = sourceName + " line " + juce::String (i + 1) + ": expected \"key\" = \"value\";";
			return false;
		}

		juce::String key;
		juce::String value;
		juce::String tokenError;

		if (! readToken (line.substring (0, equals), key, tokenError)
			|| ! readToken (line.substring (equals + 1), value, tokenError))
		{
			error = sourceName + " line " + juce::String (i + 1) + ": " + tokenError;
			return false;
		}

		key = key.trim();

		if (key.isEmpty())
		{
			error = sourceName + " line " + juce::String (i + 1) + ": empty key";
			return false;
		}

		if (entries.find (key) != entries.end())
		{
			error = sourceName + " line " + juce::String (i + 1) + ": duplicate key \"" + key + "\"";
			return false;
		}

		entries.emplace (key, value);
	}

	if (languageTag.isEmpty())
	{
		error = sourceName + ": missing `id:` (the language tag, e.g. `zh-Hans`)";
		return false;
	}

	if (displayName.isEmpty())
	{
		error = sourceName + ": missing `language:` (the name shown in the language list)";
		return false;
	}

	return true;
}

bool LanguagePack::loadFromFile (const juce::File& file, juce::String& error)
{
	if (! file.existsAsFile())
	{
		error = file.getFullPathName() + ": file not found";
		return false;
	}

	return parse (file.loadFileAsString(), file.getFileName(), error);
}

juce::String LanguagePack::find (const juce::String& key) const
{
	const auto found = entries.find (key);
	return found != entries.end() ? found->second : juce::String();
}

bool LanguagePack::contains (const juce::String& key) const
{
	return entries.find (key) != entries.end();
}

int LanguagePack::countMissing (const LanguagePack& reference) const
{
	int missing = 0;

	for (const auto& entry : reference.entries)
	{
		if (find (entry.first).isEmpty())
		{
			++missing;
		}
	}

	return missing;
}

} // namespace toybox::i18n
