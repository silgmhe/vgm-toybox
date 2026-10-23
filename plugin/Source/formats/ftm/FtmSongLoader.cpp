#include "FtmSongLoader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace toybox::formats::ftm
{
namespace
{
	/** A bounds-checked reader over one block's payload. Every field is little-endian in FTM. */
	class Reader
	{
	public:
		Reader (const std::uint8_t* data, std::size_t size) : bytes (data), length (size) {}

		bool ok (void) const { return failed == false; }
		std::size_t remaining (void) const { return position <= length ? length - position : 0; }
		std::size_t offset (void) const { return position; }

		std::uint8_t u8 (void)
		{
			if (position + 1 > length)
			{
				failed = true;
				return 0;
			}
			return bytes[position++];
		}

		std::uint32_t u32 (void)
		{
			if (position + 4 > length)
			{
				failed = true;
				return 0;
			}
			const std::uint32_t value = (std::uint32_t) bytes[position]
									  | ((std::uint32_t) bytes[position + 1] << 8)
									  | ((std::uint32_t) bytes[position + 2] << 16)
									  | ((std::uint32_t) bytes[position + 3] << 24);
			position += 4;
			return value;
		}

		int i32 (void) { return (int) u32(); }

		std::string cstring (void)
		{
			std::string text;
			while (position < length)
			{
				const char c = (char) bytes[position++];
				if (c == '\0')
				{
					break;
				}
				text.push_back (c);
			}
			return text;
		}

		/** A string the module length-prefixed. Unlike `cstring` this reads exactly what it was
			told: an instrument name carries no terminator, so stopping at the first zero byte would
			eat into the next record's index and misalign everything after it. */
		std::string fixedString (std::size_t count)
		{
			std::string text;
			for (std::size_t i = 0; i < count && position < length; ++i)
			{
				text.push_back ((char) bytes[position++]);
			}
			return text;
		}

		void skip (std::size_t count)
		{
			if (position + count > length)
			{
				failed = true;
				position = length;
				return;
			}
			position += count;
		}

	private:
		const std::uint8_t* bytes = nullptr;
		std::size_t length = 0;
		std::size_t position = 0;
		bool failed = false;
	};

	struct Block
	{
		std::string id;
		std::uint32_t version = 0;
		const std::uint8_t* payload = nullptr;
		std::size_t size = 0;
	};

	const char* const kHeader = "FamiTracker Module";
	const char* const kHeaderDn = "Dn-FamiTracker Module";

	void parseParams (const Block& block, Module& module)
	{
		Reader reader (block.payload, block.size);
		if (block.version >= 2)
		{
			(void) reader.u8();			// expansion chip
		}
		module.channels = (int) reader.u32();
		module.machine = (int) reader.u32();
	}

	void parseInfo (const Block& block, Module& module)
	{
		Reader reader (block.payload, block.size);
		module.title = reader.cstring();
		module.author = reader.cstring();
	}

	void parseHeader (const Block& block, Module& module)
	{
		Reader reader (block.payload, block.size);
		const int trackCount = (block.version >= 2) ? (int) reader.u8() + 1 : 1;

		if (block.version >= 3)
		{
			for (int track = 0; track < trackCount; ++track)
			{
				(void) reader.cstring();		// track title
			}
		}

		module.channelTypes.assign ((std::size_t) module.channels, 0);
		module.effectColumns.assign ((std::size_t) module.channels, 0);

		for (int channel = 0; channel < module.channels; ++channel)
		{
			module.channelTypes[(std::size_t) channel] = reader.u8();
			for (int track = 0; track < trackCount; ++track)
			{
				const int columns = reader.u8();
				if (track == 0)
				{
					module.effectColumns[(std::size_t) channel] = columns;
				}
			}
		}
	}

	void parseFrames (const Block& block, Module& module)
	{
		Reader reader (block.payload, block.size);
		module.order.assign ((std::size_t) module.channels, {});

		// Only the first track is transcribed; the block repeats this record per track.
		module.frameCount = reader.i32();
		module.speed = reader.i32();
		module.tempo = reader.i32();
		module.patternLength = reader.i32();

		if (module.speed <= 0)
		{
			module.speed = 6;
		}
		if (module.tempo <= 0)
		{
			module.tempo = 150;
		}
		if (module.patternLength <= 0)
		{
			module.patternLength = 64;
		}

		for (int frame = 0; frame < module.frameCount; ++frame)
		{
			for (int channel = 0; channel < module.channels; ++channel)
			{
				const int index = reader.u8();
				if (frame == 0)
				{
					module.order[(std::size_t) channel].reserve ((std::size_t) module.frameCount);
				}
				module.order[(std::size_t) channel].push_back (index);
			}
		}
	}


	void parseInstruments (const Block& block, Module& module)
	{
		Reader reader (block.payload, block.size);
		const int count = reader.i32();

		for (int i = 0; i < count && reader.ok(); ++i)
		{
			const int index = reader.i32();
			const int type = reader.u8();

			// Only the 2A03 instrument's layout is known, and a record carries no length prefix, so
			// an unknown type cannot be skipped - stop rather than read the rest as noise.
			if (type != 1)				// INST_2A03
			{
				break;
			}

			(void) reader.i32();		// sequence count, which the reference then forces to SEQ_COUNT

			int volumeSequence = -1;
			int arpeggioSequence = -1;
			int pitchSequence = -1;
			int dutySequence = -1;

			for (int sequenceType = 0; sequenceType < 5; ++sequenceType)
			{
				const int enabled = reader.u8();
				const int sequence = reader.u8();

				if (enabled == 0)
				{
					continue;
				}

				if (sequenceType == 0)			// SEQ_VOLUME
				{
					volumeSequence = sequence;
				}
				else if (sequenceType == 1)		// SEQ_ARPEGGIO
				{
					arpeggioSequence = sequence;
				}
				else if (sequenceType == 2)		// SEQ_PITCH
				{
					pitchSequence = sequence;
				}
				else if (sequenceType == 4)		// SEQ_DUTYCYCLE
				{
					dutySequence = sequence;
				}
			}

			// CInstrument2A03::Load: sample assignment per octave and note. Version 5 and below
			// stores two bytes per assignment, later versions a third; version 7 turns the dense
			// table into a counted list.
			const int bytesPerAssignment = (block.version > 5) ? 3 : 2;

			if (block.version >= 7)
			{
				const int assigned = reader.i32();
				reader.skip ((std::size_t) (assigned * bytesPerAssignment));
			}
			else
			{
				const int octaves = (block.version == 1) ? 6 : 8;
				reader.skip ((std::size_t) (octaves * 12 * bytesPerAssignment));
			}

			const std::size_t nameLength = reader.u32();
			const std::string name = reader.fixedString (nameLength);

			if (index >= 0 && index < 256)
			{
				if ((int) module.instruments.size() <= index)
				{
					module.instruments.resize ((std::size_t) index + 1);
				}

				module.instruments[(std::size_t) index].name = name;
				module.instruments[(std::size_t) index].volume = volumeSequence;
				module.instruments[(std::size_t) index].arpeggio = arpeggioSequence;
				module.instruments[(std::size_t) index].pitch = pitchSequence;
				module.instruments[(std::size_t) index].duty = dutySequence;
			}
		}
	}

	void parseSequences (const Block& block, Module& module)
	{
		Reader reader (block.payload, block.size);
		const int count = reader.i32();

		for (int i = 0; i < count && reader.ok(); ++i)
		{
			const int index = reader.i32();
			const int type = reader.i32();
			const int items = reader.u8();
			(void) reader.i32();		// loop point

			std::vector<std::uint8_t> values;
			for (int k = 0; k < items && reader.ok(); ++k)
			{
				values.push_back (reader.u8());
			}

			if (type == 0)				// SEQ_VOLUME
			{
				if ((int) module.volumeSequences.size() <= index)
				{
					module.volumeSequences.resize ((std::size_t) index + 1);
				}

				module.volumeSequences[(std::size_t) index] = std::move (values);
			}
			else if (type == 1)			// SEQ_ARPEGGIO
			{
				if ((int) module.arpeggioSequences.size() <= index)
				{
					module.arpeggioSequences.resize ((std::size_t) index + 1);
				}

				module.arpeggioSequences[(std::size_t) index] = std::move (values);
			}
			else if (type == 2)			// SEQ_PITCH
			{
				if ((int) module.pitchSequences.size() <= index)
				{
					module.pitchSequences.resize ((std::size_t) index + 1);
				}

				module.pitchSequences[(std::size_t) index] = std::move (values);
			}
			else if (type == 4)			// SEQ_DUTYCYCLE
			{
				if ((int) module.dutySequences.size() <= index)
				{
					module.dutySequences.resize ((std::size_t) index + 1);
				}

				module.dutySequences[(std::size_t) index] = std::move (values);
			}
		}
	}

	void parsePatterns (const Block& block, Module& module)
	{
		Reader reader (block.payload, block.size);

		while (reader.remaining() >= 16 && reader.ok())
		{
			const int track = reader.i32();
			const int channel = reader.i32();
			const int index = reader.i32();
			const int items = reader.i32();

			if (! reader.ok() || channel < 0 || channel >= module.channels || track != 0)
			{
				break;
			}

			ModulePattern pattern;
			pattern.channel = channel;
			pattern.index = index;

			for (int item = 0; item < items && reader.ok(); ++item)
			{
				const int row = reader.i32();
				ModuleRow note;
				note.row = row;
				note.note = reader.u8();
				note.octave = reader.u8();
				note.instrument = reader.u8();
				note.volume = reader.u8();

				const int effectColumns = (channel < (int) module.effectColumns.size())
											? module.effectColumns[(std::size_t) channel] : 0;
				for (int column = 0; column <= effectColumns; ++column)
				{
					const int number = reader.u8();
					const int param = reader.u8();
					if (number != 0)
					{
						note.effects.emplace_back (number, param);
					}
				}

				if (note.hasNote() || ! note.effects.empty())
				{
					pattern.rows.push_back (note);
				}
			}

			module.patterns.push_back (std::move (pattern));
		}
	}
} // namespace

double Module::framesPerSecond (void) const
{
	// FamiTracker's tempo is 2.5 frames per unit: 150 is 60 Hz, which is the NTSC frame rate, and
	// speed is how many of those frames a row lasts.
	return (double) tempo / 2.5;
}

bool parseModule (const void* data, std::size_t size, Module& out, std::string& error)
{
	const auto* bytes = static_cast<const std::uint8_t*> (data);
	std::size_t position = 0;

	std::size_t headerLength = 0;
	if (size >= std::strlen (kHeader) && std::memcmp (bytes, kHeader, std::strlen (kHeader)) == 0)
	{
		headerLength = std::strlen (kHeader);
	}
	else if (size >= std::strlen (kHeaderDn)
			 && std::memcmp (bytes, kHeaderDn, std::strlen (kHeaderDn)) == 0)
	{
		headerLength = std::strlen (kHeaderDn);
	}
	else
	{
		error = "not a FamiTracker module";
		return false;
	}

	position = headerLength + 4;		// the file version, which nothing below needs

	Block params;
	Block info;
	Block header;
	Block frames;
	Block patterns;
	Block instruments;
	Block sequences;
	bool sawEnd = false;

	while (position + 24 <= size)
	{
		Block block;
		char id[17] = {};
		std::memcpy (id, bytes + position, 16);
		block.id = id;

		std::memcpy (&block.version, bytes + position + 16, 4);
		std::uint32_t blockSize = 0;
		std::memcpy (&blockSize, bytes + position + 20, 4);
		position += 24;

		if (blockSize > size - position)
		{
			error = "truncated block " + block.id;
			return false;
		}

		block.payload = bytes + position;
		block.size = blockSize;
		position += blockSize;

		if (block.id == "PARAMS")		params = block;
		else if (block.id == "INFO")	info = block;
		else if (block.id == "HEADER")	header = block;
		else if (block.id == "FRAMES")	frames = block;
		else if (block.id == "PATTERNS") patterns = block;
		else if (block.id == "INSTRUMENTS") instruments = block;
		else if (block.id == "SEQUENCES") sequences = block;
	}

	// The reference writes a bare "END" after the last block rather than another block record.
	sawEnd = size >= 3 && std::memcmp (bytes + size - 3, "END", 3) == 0;

	if (header.payload == nullptr || frames.payload == nullptr || patterns.payload == nullptr)
	{
		error = "missing a required block";
		return false;
	}

	if (params.payload != nullptr)
	{
		parseParams (params, out);
	}

	if (info.payload != nullptr)
	{
		parseInfo (info, out);
	}

	parseHeader (header, out);
	parseFrames (frames, out);
	parsePatterns (patterns, out);

	if (instruments.payload != nullptr)
	{
		parseInstruments (instruments, out);
	}

	if (sequences.payload != nullptr)
	{
		parseSequences (sequences, out);
	}

	if (out.channels <= 0 || out.channelTypes.empty())
	{
		error = "the header names no channels";
		return false;
	}

	if (! sawEnd)
	{
		error = "the module has no END block";
		return false;
	}

	return true;
}

namespace
{
	chips::ChannelKind kindForChannel (int channelType)
	{
		switch (channelType)
		{
		case 0: case 1: return chips::ChannelKind::nesPulse;
		case 2:			return chips::ChannelKind::nesTriangle;
		case 3:			return chips::ChannelKind::nesNoise;
		case 4:			return chips::ChannelKind::nesDmc;
		default:		return chips::ChannelKind::unknown;
		}
	}

	const char* nameForChannel (int channelType)
	{
		switch (channelType)
		{
		case 0:			return "Pulse 1";
		case 1:			return "Pulse 2";
		case 2:			return "Triangle";
		case 3:			return "Noise";
		case 4:			return "DPCM";
		default:		return "Channel";
		}
	}
} // namespace

bool loadNoteStream (const void* data, std::size_t size, midi::NoteStream& out, std::string& error)
{
	Module module;
	if (! parseModule (data, size, module, error))
	{
		return false;
	}

	out = midi::NoteStream {};
	// The time base is the chip's own frame rate, not the row: FamiTracker's speed says how many
	// frames a row lasts, and its effects step per frame. Measuring in frames is what lets a bend
	// land where the tracker puts it.
	out.tickRate = module.framesPerSecond();

	std::map<std::pair<int, int>, const ModulePattern*> lookup;
	for (const auto& pattern : module.patterns)
	{
		lookup[{ pattern.channel, pattern.index }] = &pattern;
	}

	// One track per channel that appears in the order list.
	std::vector<int> channels;
	std::vector<int> trackOf ((std::size_t) module.channels, -1);

	for (int channel = 0; channel < module.channels; ++channel)
	{
		if (module.order[(std::size_t) channel].empty())
		{
			continue;
		}

		const int channelType = module.channelTypes[(std::size_t) channel];
		midi::StreamTrack described;
		described.name = nameForChannel (channelType);
		described.kind = kindForChannel (channelType);
		described.isDrum = (channelType == 3 || channelType == 4);

		trackOf[(std::size_t) channel] = (int) out.tracks.size();
		channels.push_back (channel);
		out.tracks.push_back (described);
	}

	// Frames are walked, not played straight through: Bxx and Dxx are the song's structure, and
	// ignoring them transcribes the file as if it had no repeats and no skips. A module can loop
	// forever; a transcription cannot, so the walk stops after four passes' worth of frames.
	const int maxSteps = module.frameCount * 4 + 64;
	int frame = 0;
	std::uint32_t frameBase = 0;

	// Frames per row, which Fxx may change at any row and which then carries into later frames.
	std::uint32_t rowFrames = (std::uint32_t) module.speed;

	// The note each channel is sounding, which 3xx needs: it slides from here to the note it carries.
	std::vector<int> sounding ((std::size_t) module.channels, -1);

	for (int step = 0; frame >= 0 && frame < module.frameCount && step < maxSteps; ++step)
	{
		int nextFrame = frame + 1;

		std::map<int, std::uint32_t> speedAtRow;
		for (const int channel : channels)
		{
			const auto found = lookup.find ({ channel, module.order[(std::size_t) channel][(std::size_t) frame] });
			if (found == lookup.end())
			{
				continue;
			}

			for (const auto& row : found->second->rows)
			{
				for (const auto& effect : row.effects)
				{
					if (effect.first == 1 && effect.second > 0)
					{
						speedAtRow[row.row] = (std::uint32_t) effect.second;
					}
				}
			}
		}

		std::vector<std::uint32_t> offsets ((std::size_t) module.patternLength + 1, 0);
		std::uint32_t clock = rowFrames;
		for (int row = 0; row < module.patternLength; ++row)
		{
			// The effect executes at its own row, so that row already lasts the new speed - which is
			// what the reference's scan does (FamiTrackerDoc.cpp:4953 sets Speed, then advances).
			const auto found = speedAtRow.find (row);
			if (found != speedAtRow.end())
			{
				clock = found->second;
			}

			offsets[(std::size_t) row + 1] = offsets[(std::size_t) row] + clock;
		}
		rowFrames = clock;

		for (const int channel : channels)
		{
			const auto found = lookup.find ({ channel, module.order[(std::size_t) channel][(std::size_t) frame] });
			if (found == lookup.end())
			{
				continue;
			}

			const ModulePattern& pattern = *found->second;
			const std::uint32_t base = frameBase;
			const int track = trackOf[(std::size_t) channel];
			const int channelType = module.channelTypes[(std::size_t) channel];
			const auto kind = kindForChannel (channelType);

			std::vector<const ModuleRow*> notes;
			for (const auto& row : pattern.rows)
			{
				if (row.hasNote())
				{
					notes.push_back (&row);
				}
			}

			for (std::size_t i = 0; i < notes.size(); ++i)
			{
				const ModuleRow& note = *notes[i];
				// read before this note updates it: 3xx slides from whatever was sounding
				const int previousNote = sounding[(std::size_t) channel];

				chips::NoteEvent event;
				event.startSample = base + offsets[(std::size_t) note.row];
				event.endSample = (i + 1 < notes.size())
									  ? base + offsets[(std::size_t) notes[i + 1]->row]
									  : base + offsets[(std::size_t) module.patternLength];

				const int midiNote = 12 + note.octave * 12 + (note.note - 1);
				event.note = (std::uint8_t) std::max (0, std::min (127, midiNote));
				// FamiTracker writes a level in 0..15 in two places: the volume column, and the first
				// item of an instrument's volume envelope. MIDI wants 0..127. Neither being present
				// is not silence - the note plays, the tracker just had nothing to say about how
				// loud - so it gets the conventional default rather than a velocity of zero.
				int level = note.volume;
				bool stated = level > 0;

				if (! stated && note.instrument > 0
					&& note.instrument < (int) module.instruments.size())
				{
					const int sequence = module.instruments[(std::size_t) note.instrument].volume;

					if (sequence >= 0 && sequence < (int) module.volumeSequences.size())
					{
						const auto& items = module.volumeSequences[(std::size_t) sequence];
						if (! items.empty())
						{
							level = items.front();
							stated = true;
						}
					}
				}

				const int velocity = stated ? ((level * 127 + 7) / 15) : 100;

				event.velocity = (std::uint8_t) std::max (0, std::min (127, velocity));
				event.logicalChannel = channel;
				event.channelKind = kind;
				event.patch = note.instrument;
				event.isDrum = (channelType == 3 || channelType == 4);
				event.pitchSource = chips::PitchSource::registerExact;	// named, not inferred

				// Sxx (effect 23) cuts the note: the row it sits on ends the sound, `xx` frames later.
				// Without it a note that the tracker silences early keeps ringing in the MIDI.
				for (const auto& row : pattern.rows)
				{
					if (row.row < note.row || row.hasNote())
					{
						continue;
					}

					for (const auto& effect : row.effects)
					{
						if (effect.first == 23)
						{
							const std::uint32_t cut = base
													+ offsets[(std::size_t) row.row]
													+ (std::uint32_t) (effect.second & 0x0F);
							if (cut < event.endSample)
							{
								event.endSample = cut;
							}
						}
					}
				}

				if (event.endSample <= event.startSample)
				{
					continue;		// cut before it began: nothing to hear
				}

				// The instrument's arpeggio macro: semitone offsets stepped one per frame, from the
				// note's own pitch. Same shape as 0xy and the same output, but the numbers come from
				// the instrument rather than from a row - which is where NES music usually keeps them.
				if (note.instrument > 0 && note.instrument < (int) module.instruments.size())
				{
					const int sequence = module.instruments[(std::size_t) note.instrument].arpeggio;

					if (sequence >= 0 && sequence < (int) module.arpeggioSequences.size())
					{
						const auto& items = module.arpeggioSequences[(std::size_t) sequence];

						if (! items.empty())
						{
							std::size_t index = 1;

							for (std::uint32_t frame = event.startSample + 1; frame < event.endSample; ++frame)
							{
								chips::PitchEvent bend;
								bend.sampleTime = frame;
								bend.logicalChannel = channel;
								bend.cents = (std::int16_t) ((std::int8_t) items[index % items.size()] * 100);
								out.bends.push_back ({ track, bend });
								++index;
							}
						}
					}
				}

				out.notes.push_back ({ track, event });
				sounding[(std::size_t) channel] = (int) event.note;

				// 0xy arpeggio: the pitch steps through +0, +x, +y semitones, one step per row.
				// A row is `speed` frames on the chip, so this is coarser than the hardware - it is
				// the closest a note-per-row transcription gets without splitting a note into ticks.
				for (const auto& effect : note.effects)
				{
					if (effect.first == 10)
					{
						const int steps[3] = { 0, (effect.second >> 4) & 0x0F, effect.second & 0x0F };
						std::size_t step = 1;

						for (std::uint32_t row = event.startSample + 1; row < event.endSample; ++row)
						{
							chips::PitchEvent bend;
							bend.sampleTime = row;
							bend.logicalChannel = channel;
							bend.cents = (std::int16_t) (steps[step % 3] * 100);
							out.bends.push_back ({ track, bend });
							++step;
						}
					}
					else if (effect.first == 16 || effect.first == 17)
					{
						// 1xx porta up / 2xx porta down, kept symbolic: the tracker slides the
						// channel's period by `xx` per frame, and what that is worth in cents depends
						// on the chip's divider and on the note it starts from. The encoder prices it.
						chips::SlideEvent slide;
						slide.startSample = event.startSample;
						slide.endSample = event.endSample;
						slide.fromNote = (int) event.note;
						slide.unitsPerFrame = (effect.first == 16 ? 1 : -1) * (effect.second & 0xFF);
						slide.logicalChannel = channel;
						slide.channelKind = kind;
						out.slides.push_back ({ track, slide });
					}
					else if (effect.first == 6)
					{
						// 3xx tone portamento: walk from the sounding note to this one. The row still
						// produces its own note - MIDI has no "arrive without retriggering" - so the
						// bend is what carries the ear across, and the note-on marks the arrival point.
						const int previous = previousNote;
						if (previous > 0 && previous != (int) event.note)
						{
							chips::SlideEvent slide;
							slide.startSample = event.startSample;
							slide.endSample = event.endSample;
							slide.fromNote = previous;
							slide.targetNote = (int) event.note;
							slide.unitsPerFrame = (event.note > previous ? 1 : -1) * (effect.second & 0xFF);
							slide.logicalChannel = channel;
							slide.channelKind = kind;
							out.slides.push_back ({ track, slide });
						}
					}
					else if (effect.first == 11)
					{
						// 4xy vibrato: x is the speed in table steps, y the depth. FamiTracker walks a
						// 64-step sine and scales it by y/16 semitone. One step per row here, because a
						// row is the finest thing this transcription has - the same approximation the
						// arpeggio above states.
						const double depth = (effect.second & 0x0F) / 16.0;
						const int speed = (effect.second >> 4) & 0x0F;
						int phase = speed;

						for (std::uint32_t row = event.startSample + 1; row < event.endSample; ++row)
						{
							const double sine = std::sin (2.0 * 3.14159265358979323846
														  * (double) (phase % 64) / 64.0);
							chips::PitchEvent bend;
							bend.sampleTime = row;
							bend.logicalChannel = channel;
							bend.cents = (std::int16_t) std::lround (100.0 * depth * sine);
							out.bends.push_back ({ track, bend });
							phase += speed;
						}
					}
				}
			}

			// Structure effects (0.5.0 numbering): 2 = jump, 3 = skip, 4 = halt.
			for (const auto& row : pattern.rows)
			{
				for (const auto& effect : row.effects)
				{
					switch (effect.first)
					{
					case 2:		nextFrame = effect.second; break;
					case 3:		nextFrame = frame + 1 + effect.second; break;
					case 4:		nextFrame = -1; break;
					default:	break;
					}
				}
			}
		}

		// A jump that goes backwards is the song's loop; one pass is what a transcription wants,
		// so the walk stops there instead of repeating the material four times.
		if (nextFrame <= frame)
		{
			nextFrame = -1;
		}

		frameBase += offsets[(std::size_t) module.patternLength];
		frame = nextFrame;
	}

	return ! out.notes.empty();
}

} // namespace toybox::formats::ftm
