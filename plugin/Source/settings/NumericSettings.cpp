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

		// The two stages of a rendered FM note. Milliseconds rather than seconds so they sit in
		// this table with the others instead of needing one of their own.
		{ "fm.sustainMs", "settings.visualiser.fmSustainLength", "",
		  fm::minSustainMs, fm::maxSustainMs, fm::sustainStepMs,
		  fm::defaultSustainMs, " ms" },

		{ "fm.releaseMs", "settings.visualiser.fmReleaseLength", "",
		  fm::minReleaseMs, fm::maxReleaseMs, fm::releaseStepMs,
		  fm::defaultReleaseMs, " ms" },

		// The note an export renders at when the user pins one. A piano's range, because the
		// control exists to pick a playable note rather than to sweep the register.
		{ "fm.note", "settings.visualiser.fmFixedNote", "",
		  fm::minFixedNote, fm::maxFixedNote, 1,
		  fm::defaultFixedNote, "" },
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
