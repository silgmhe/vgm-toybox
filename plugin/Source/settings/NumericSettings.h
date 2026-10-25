#pragma once

#include <vector>

namespace toybox::settings
{

/** One numeric preference: its key, its bounds, and the words that describe it.

	Adding a preference used to mean a key string, a getter, a setter, a label, a slider with its
	range written out again, a line of layout and two translations - eight places, each of which
	could disagree with the others about the range. The table is the one description; the store
	reads and writes through it, and a page that wants a slider asks for its bounds instead of
	repeating them.
*/
struct NumericSetting
{
	const char* key;			// the storage key, e.g. "nsf.outputGainDb"
	const char* labelKey;		// i18n key for the caption
	const char* tooltipKey;		// i18n key for the explanation, or "" for none
	int minimum = 0;
	int maximum = 100;
	int step = 1;
	int defaultValue = 0;
	const char* suffix = "";	// " ms", " dB", ...
};

/** Every numeric preference, in the order the pages show them. */
const std::vector<NumericSetting>& numericSettings (void);

/** The descriptor for `key`, or nullptr. Used by pages that only know their key. */
const NumericSetting* numericSetting (const char* key);

} // namespace toybox::settings
