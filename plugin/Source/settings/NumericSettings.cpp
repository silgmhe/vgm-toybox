#include "NumericSettings.h"

#include "KeyboardSettings.h"

#include "playback/MidiSequence.h"

#include <cstring>

namespace toybox::settings
{

const std::vector<NumericSetting>& numericSettings (void)
{
	// The keys are the ones already in settings.xml: they are written out rather than derived from
	// the names, because renaming one silently resets everybody's preference.
	//
	// The bounds are the ones the rest of the program uses - the MIDI bend range comes from the
	// sequencer that has to obey it, the note-off delay from the keyboard page's own header - so
	// there is no second copy of a range to drift.
	static const std::vector<NumericSetting> table
	{
		{ "midi.pitchBendRange", "settings.midi.range", "settings.midi.rangeHint",
		  playback::minPitchBendRange, playback::maxPitchBendRange, 1,
		  playback::defaultPitchBendRange, "" },

		{ "keyboard.noteHoldMs", "settings.keyboard.noteHold", "",
		  keyboard::minNoteHoldMs, keyboard::maxNoteHoldMs, keyboard::noteHoldStepMs,
		  keyboard::defaultNoteHoldMs, " ms" },
	};

	return table;
}

const NumericSetting* numericSetting (const char* key)
{
	for (const auto& setting : numericSettings ())
	{
		if (std::strcmp (setting.key, key) == 0)
		{
			return &setting;
		}
	}

	return nullptr;
}

} // namespace toybox::settings
