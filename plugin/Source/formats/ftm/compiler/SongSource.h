#pragma once

// The seam between FamiTracker's compiler and this project's module model.
//
// FamiTracker's Compiler.cpp is 3,057 lines and reaches into its document 117 times through 37
// distinct methods. Porting it by editing those call sites in place would scatter 117 changes
// through code we did not write; this interface collects them in one place instead, so the port is
// a mechanical substitution (`m_pDocument->X()` becomes `m_source->X()`) and the only new code is
// the adapter that implements it over `formats::ftm::Module`.
//
// The signatures follow FamiTrackerDoc.h deliberately, including its warts: `GetPatternAtFrame`
// takes a track and a channel, `GetDataAtPattern` returns a pointer into the document's pattern
// array, and the instrument getters hand back FamiTracker's own instrument objects. Changing the
// shape here would mean changing the compiler, which is the thing we are trying to port untouched.
//
// Status: the interface is declared, and `ModuleSongSource` will implement it in stages. Methods
// marked `[module]` are already backed by what the loader parses today; the rest need data the
// loader does not read yet (instrument sequences by type, DPCM assignments, grooves, detune tables,
// expansion-chip parameters) and are the reason the port is staged.

#include <cstdint>
#include <string>
#include <vector>

namespace toybox::formats::ftm::compiler
{

class CInstrument;			// FamiTracker's instrument, ported alongside the compiler

/** One pattern cell, laid out as FamiTracker's `stChanNote` is (PatternNote.h:27) so the compiler
	can keep reading cells the way it always has.

	The defaults are FamiTracker's own sentinels, not zero: an empty cell holds NONE for the note,
	MAX_VOLUME for the volume and MAX_INSTRUMENTS for the instrument. Zeroing them instead would
	turn "nothing here" into "play C-0 at volume 0", which the compiler would happily encode. */
struct stChanNote
{
	static constexpr unsigned char NONE = 0;
	static constexpr unsigned char MAX_VOLUME = 0x10;
	static constexpr unsigned char MAX_INSTRUMENTS = 0x40;
	static constexpr int MAX_EFFECT_COLUMNS = 4;

	unsigned char Note = NONE;
	unsigned char Octave = 0;
	unsigned char Vol = MAX_VOLUME;
	unsigned char Instrument = MAX_INSTRUMENTS;
	unsigned char EffNumber[MAX_EFFECT_COLUMNS] = {};
	unsigned char EffParam[MAX_EFFECT_COLUMNS] = {};
};

/** Everything the compiler asks a module, in one place. */
class ISongSource
{
public:
	virtual ~ISongSource() = default;

	// --- song shape -------------------------------------------------------------------------
	virtual int GetPatternLength() const = 0;						// [module]
	virtual int GetFrameCount() const = 0;							// [module]
	virtual int GetChannelCount() const = 0;						// [module]
	virtual int GetAvailableChannels() const = 0;					// [module]
	virtual int GetTrackCount() const = 0;							// [module] (always 1 for now)
	virtual int GetChannelType (int channel) const = 0;				// [module]
	virtual int GetChannelIndex (int track, int channel) const = 0;	// [module]

	// --- arrangement ------------------------------------------------------------------------
	virtual int GetPatternAtFrame (int track, int frame, int channel) const = 0;	// [module]
	virtual const stChanNote* GetDataAtPattern (int track, int channel, int pattern, int row) const = 0;

	// --- timing -----------------------------------------------------------------------------
	virtual int GetSongSpeed() const = 0;							// [module] initial speed
	virtual int GetSongTempo() const = 0;							// [module] initial tempo
	virtual int GetEngineSpeed() const = 0;							// speed after Fxx has run
	virtual int GetSongGroove() const = 0;
	virtual int GetGroove (int index, int row) const = 0;
	virtual bool GetLinearPitch() const = 0;
	virtual int GetVibratoStyle() const = 0;

	// --- instruments and samples ------------------------------------------------------------
	virtual CInstrument* GetInstrument (int index) const = 0;
	virtual int GetInstrumentType (int index) const = 0;
	virtual bool IsInstrumentUsed (int index) const = 0;
	virtual bool IsSampleUsed (int index) const = 0;
	virtual int GetSample (int index, int& pitch, int& delta) const = 0;

	// --- expansion chips --------------------------------------------------------------------
	virtual int GetExpansionChip() const = 0;						// [module] chip mask
	virtual bool ExpansionEnabled (int chip) const = 0;				// [module]
	virtual void SelectExpansionChip (int chip) = 0;
	virtual bool GetExternalOPLLChipCheck() const = 0;
	virtual int GetNamcoChannels() const = 0;
	virtual void SetNamcoChannels (int channels) = 0;
	virtual int GetOPLLPatchByte (int index) const = 0;

	// --- metadata ---------------------------------------------------------------------------
	virtual std::string GetSongName() const = 0;					// [module] INFO
	virtual std::string GetSongArtist() const = 0;					// [module]
	virtual std::string GetSongCopyright() const = 0;				// [module]
	virtual std::string GetComment() const = 0;
	virtual std::string GetTitle() const = 0;						// [module]
	virtual std::string GetTrackTitle (int track) const = 0;		// [module]
	virtual int GetLevelOffset() const = 0;
	virtual int GetStandardLength() const = 0;
};

} // namespace toybox::formats::ftm::compiler
