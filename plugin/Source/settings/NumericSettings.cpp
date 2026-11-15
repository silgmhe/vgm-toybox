#include "NumericSettings.h"

#include "SettingsDefaults.h"

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
		  midi::minPitchBendRange, midi::maxPitchBendRange, 1,
		  midi::pitchBendRange, "" },

		{ "keyboard.noteHoldMs", "settings.keyboard.noteHold", "",
		  keyboard::minNoteHoldMs, keyboard::maxNoteHoldMs, keyboard::noteHoldStepMs,
		  keyboard::defaultNoteHoldMs, " ms" },

		// How long an FM note is rendered for. Milliseconds rather than seconds so it can sit in
		// this table with the others instead of needing a table of its own - the settings page
		// shows the number, and 50 ms steps are finer than anyone sets it by.
		{ "visualiser.fmNoteMs", "settings.visualiser.fmNoteLength", "",
		  visualiser::minNoteMs, visualiser::maxNoteMs, visualiser::noteStepMs,
		  visualiser::defaultNoteMs, " ms" },

		// The note an export renders at when the user pins one. A piano's range, because the
		// control exists to pick a playable note rather than to sweep the register.
		{ "visualiser.fmFixedNote", "settings.visualiser.fmFixedNote", "",
		  visualiser::minFixedNote, visualiser::maxFixedNote, 1,
		  visualiser::defaultFixedNote, "" },
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
