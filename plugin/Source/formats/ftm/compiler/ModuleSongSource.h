#pragma once

// The adapter: FamiTracker's compiler interface answered from this project's Module.
//
// Everything the loader already parses is implemented here; everything that needs data we do not
// read yet returns a documented default rather than a guess, because a wrong answer would surface
// as audio that is subtly wrong instead of missing.
//
// Not wired into anything yet - it compiles, and the tests exercise the accessors, which is what
// makes the next step (moving Compiler.cpp over and substituting its calls) mechanical.

#include <string>

#include "formats/ftm/FtmSongLoader.h"
#include "formats/ftm/compiler/SongSource.h"

namespace toybox::formats::ftm::compiler
{

class ModuleSongSource final : public ISongSource
{
public:
	explicit ModuleSongSource (const Module& module) : song (module) {}

	// --- song shape -------------------------------------------------------------------------
	int GetPatternLength() const override { return song.patternLength; }
	int GetFrameCount() const override { return song.frameCount; }
	int GetChannelCount() const override { return song.channels; }

	/** Channels that carry an order list. A module can declare more channels than it uses, and the
		compiler only needs code for the ones that play. */
	int GetAvailableChannels() const override
	{
		int used = 0;
		for (const auto& order : song.order)
		{
			if (! order.empty())
			{
				++used;
			}
		}
		return used;
	}

	int GetTrackCount() const override { return 1; }

	int GetChannelType (int channel) const override
	{
		return (channel >= 0 && channel < (int) song.channelTypes.size())
				   ? song.channelTypes[(std::size_t) channel] : 0;
	}

	/** The channel's own number. FamiTracker's track/channel pair collapses to one here because a
		module this loader reads has a single track - the pattern records from other tracks are read
		and discarded, not merged. */
	int GetChannelIndex (int track, int channel) const override
	{
		(void) track;
		return (channel >= 0 && channel < song.channels) ? channel : -1;
	}

	// --- arrangement ------------------------------------------------------------------------
	int GetPatternAtFrame (int track, int frame, int channel) const override
	{
		(void) track;
		if (channel < 0 || channel >= (int) song.order.size())
		{
			return -1;
		}

		const auto& order = song.order[(std::size_t) channel];
		return (frame >= 0 && frame < (int) order.size()) ? order[(std::size_t) frame] : -1;
	}

	/** FamiTracker hands the compiler a pointer into its own pattern array. This model does not
		store cells in that form, so the port has to either materialise the array or teach the
		compiler to ask per row - a decision that belongs with the port, not with this file. */
	const stChanNote* GetDataAtPattern (int, int, int, int) const override { return nullptr; }

	// --- timing -----------------------------------------------------------------------------
	int GetSongSpeed() const override { return song.speed; }
	int GetSongTempo() const override { return song.tempo; }
	int GetEngineSpeed() const override { return song.speed; }
	int GetSongGroove() const override { return 0; }
	int GetGroove (int, int) const override { return 0; }
	bool GetLinearPitch() const override { return false; }
	int GetVibratoStyle() const override { return 0; }

	// --- instruments ------------------------------------------------------------------------
	CInstrument* GetInstrument (int) const override { return nullptr; }
	int GetInstrumentType (int) const override { return 0; }
	bool IsInstrumentUsed (int index) const override
	{
		return index > 0 && index < (int) song.instruments.size()
			   && ! song.instruments[(std::size_t) index].name.empty();
	}
	bool IsSampleUsed (int) const override { return false; }
	int GetSample (int, int&, int&) const override { return -1; }

	// --- expansion chips --------------------------------------------------------------------
	int GetExpansionChip() const override { return 0; }
	bool ExpansionEnabled (int) const override { return false; }
	void SelectExpansionChip (int) override {}
	bool GetExternalOPLLChipCheck() const override { return false; }
	int GetNamcoChannels() const override { return 0; }
	void SetNamcoChannels (int) override {}
	int GetOPLLPatchByte (int) const override { return 0; }

	// --- metadata ---------------------------------------------------------------------------
	std::string GetSongName() const override { return song.title; }
	std::string GetSongArtist() const override { return song.author; }
	std::string GetSongCopyright() const override { return {}; }
	std::string GetComment() const override { return {}; }
	std::string GetTitle() const override { return song.title; }
	std::string GetTrackTitle (int) const override { return song.title; }
	int GetLevelOffset() const override { return 0; }
	int GetStandardLength() const override { return song.frameCount; }

private:
	const Module& song;
};

} // namespace toybox::formats::ftm::compiler
