#include "FtmSongLoader.h"

#include <algorithm>
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

double Module::rowsPerSecond (void) const
{
	const double framesPerSecond = (double) tempo / 2.5;
	return framesPerSecond / (double) (speed > 0 ? speed : 6);
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
	out.tickRate = module.rowsPerSecond();

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
	std::uint32_t base = 0;

	for (int step = 0; frame >= 0 && frame < module.frameCount && step < maxSteps; ++step)
	{
		int nextFrame = frame + 1;

		for (const int channel : channels)
		{
			const auto found = lookup.find ({ channel, module.order[(std::size_t) channel][(std::size_t) frame] });
			if (found == lookup.end())
			{
				continue;
			}

			const ModulePattern& pattern = *found->second;
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

				chips::NoteEvent event;
				event.startSample = base + (std::uint32_t) note.row;
				event.endSample = (i + 1 < notes.size())
									  ? base + (std::uint32_t) notes[i + 1]->row
									  : base + (std::uint32_t) module.patternLength;

				const int midiNote = 12 + note.octave * 12 + (note.note - 1);
				event.note = (std::uint8_t) std::max (0, std::min (127, midiNote));
				event.velocity = (std::uint8_t) std::max (0, std::min (127, note.volume));
				event.logicalChannel = channel;
				event.channelKind = kind;
				event.patch = note.instrument;
				event.isDrum = (channelType == 3 || channelType == 4);
				event.pitchSource = chips::PitchSource::registerExact;	// named, not inferred

				out.notes.push_back ({ track, event });
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

		base += (std::uint32_t) module.patternLength;
		frame = nextFrame;
	}

	return ! out.notes.empty();
}

} // namespace toybox::formats::ftm
