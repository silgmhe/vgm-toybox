#pragma once

// What FamiTracker's compiler asks an instrument, and nothing more.
//
// Measured from Compiler.cpp: it reaches into instruments 13 times through 11 methods - type,
// sequence enable/index/data, the DPCM assignment quartet, and the two FDS/N163 wave calls. Porting
// FamiTracker's instrument class hierarchy would bring a great deal the compiler never touches, so
// this is the surface, not the hierarchy.
//
// `Compile` is the exception and is deliberately absent for now: it is where the per-chip encoding
// lives (hundreds of lines in Instrument2A03.cpp), and it belongs with the compiler port rather
// than with the data model.

#include <cstdint>
#include <vector>

#include <string>

#include "formats/ftm/FtmSongLoader.h"
#include "formats/ftm/compiler/Chunk.h"

namespace toybox::formats::ftm::compiler
{

class CInstrument
{
public:
	virtual ~CInstrument() = default;

	/** The instrument's chip type, as FamiTracker numbers them: 1 is INST_2A03. */
	virtual int GetType() const = 0;

	/** Sequence slots are FamiTracker's SEQ_ order: volume, arpeggio, pitch, hi-pitch, duty. */
	virtual bool GetSeqEnable (int sequenceType) const = 0;
	virtual int GetSeqIndex (int sequenceType) const = 0;
	virtual const std::vector<std::uint8_t>* GetSequence (int sequenceType) const = 0;

	virtual int GetSampleIndex (int, int) const { return 0; }
	virtual int GetSamplePitch (int, int) const { return 0; }
	virtual int GetSampleDeltaValue (int, int) const { return 0; }

	virtual bool IsWaveEqual (const CInstrument*) const { return false; }
	virtual void StoreWave (std::vector<std::uint8_t>&) const {}

	/** Writes this instrument into the driver and returns the bytes it wrote.

		Two bytes of header - the chip type and a bitmask of which sequences are present - then one
		16-bit reference per present sequence, pointing at where that sequence will be laid out. */
	virtual int Compile (Chunk& chunk, int index) = 0;
};

/** A 2A03 instrument as this project models it.

	The module keeps one sequence table per type, each with its own index space, so the instrument
	points back at the module rather than copying data: `GetSeqIndex` returns a number and
	`GetSequence` resolves it against the table that type uses.

	The DPCM quartet answers zero because the loader does not read sample assignments yet - the same
	"documented default rather than a plausible guess" rule the song source follows. */
class Seq2A03Instrument final : public CInstrument
{
public:
	Seq2A03Instrument (const Module& module, int instrumentIndex)
		: song (module), index (instrumentIndex)
	{
	}

	int GetType() const override { return 1; }

	bool GetSeqEnable (int sequenceType) const override { return seqIndex (sequenceType) >= 0; }

	int GetSeqIndex (int sequenceType) const override { return seqIndex (sequenceType); }

	const std::vector<std::uint8_t>* GetSequence (int sequenceType) const override
	{
		const int sequence = seqIndex (sequenceType);
		if (sequence < 0)
		{
			return nullptr;
		}

		const std::vector<std::vector<std::uint8_t>>* table = tableFor (sequenceType);
		if (table == nullptr || sequence >= (int) table->size())
		{
			return nullptr;
		}

		const auto& items = (*table)[(std::size_t) sequence];
		return items.empty() ? nullptr : &items;
	}

	int Compile (Chunk& chunk, int) override
	{
		// The chip type, then which sequences this instrument actually has. FamiTracker writes the
		// same two bytes and then one reference per present sequence, in SEQ_ order.
		chunk.StoreByte (0);					// the driver's code for INST_2A03

		unsigned mask = 0;
		for (int type = 0; type < 5; ++type)
		{
			if (GetSeqEnable (type) && GetSequence (type) != nullptr)
			{
				mask |= 1u << type;
			}
		}

		chunk.StoreByte ((std::uint8_t) mask);

		int stored = 2;
		for (int type = 0; type < 5; ++type)
		{
			if ((mask & (1u << type)) == 0)
			{
				continue;
			}

			chunk.StoreReference ("ft_seq_2a03_"
								  + std::to_string (GetSeqIndex (type) * 5 + type));
			stored += 2;
		}

		return stored;
	}

private:
	int seqIndex (int sequenceType) const
	{
		if (index <= 0 || index >= (int) song.instruments.size())
		{
			return -1;
		}

		const auto& instrument = song.instruments[(std::size_t) index];
		switch (sequenceType)
		{
		case 0: return instrument.volume;
		case 1: return instrument.arpeggio;
		case 2: return instrument.pitch;
		case 4: return instrument.duty;
		default: return -1;
		}
	}

	const std::vector<std::vector<std::uint8_t>>* tableFor (int sequenceType) const
	{
		switch (sequenceType)
		{
		case 0: return &song.volumeSequences;
		case 1: return &song.arpeggioSequences;
		case 2: return &song.pitchSequences;
		case 4: return &song.dutySequences;
		default: return nullptr;
		}
	}

	const Module& song;
	int index = -1;
};

} // namespace toybox::formats::ftm::compiler
