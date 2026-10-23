#include "FormatsTests.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../Support/TestSupport.h"
#include "chips/ChipTypes.h"
#include "formats/ftm/FtmSongIr.h"
#include "formats/ftm/FtmSongLoader.h"
#include "formats/ftm/compiler/Chunk.h"
#include "formats/ftm/compiler/DriverData2A03.h"
#include "formats/ftm/compiler/ModuleSongSource.h"
#include "formats/vgm/VgmSongLoader.h"
#include "midi/NoteStream.h"
#include "playback/MidiSequence.h"

namespace toybox::test
{
using toybox::test::check;

	void testVgmLoaderIr (void)
	{
		std::vector<std::uint8_t> image (0x100, 0);
		image[0] = 'V'; image[1] = 'g'; image[2] = 'm'; image[3] = ' ';

		const auto put32 = [&image] (std::size_t at, std::uint32_t value)
		{
			for (int i = 0; i < 4; ++i)
			{
				image[at + (std::size_t) i] = (std::uint8_t) ((value >> (8 * i)) & 0xFF);
			}
		};

		put32 (0x08, 0x150);					// version 1.50
		put32 (0x34, 0x100 - 0x34);				// data starts right after the header block

		std::vector<std::uint8_t> data;
		const auto emit = [&data] (std::initializer_list<std::uint8_t> bytes)
		{
			data.insert (data.end(), bytes.begin(), bytes.end());
		};

		emit ({ 0x50, 0x90 });			// SN76489: register 0, value 0x90
		emit ({ 0x52, 0x28, 0xF0 });			// OPN2 port 0
		emit ({ 0x53, 0x2A, 0x11 });			// OPN2 port 1
		emit ({ 0xE1, 0x00, 0x80, 0x12, 0x34 });	// C352: offset 0x0080, value 0x1234

		// one ROM block: type 0x8D, romSize 0x100, placed at 0, sixteen payload bytes
		std::vector<std::uint8_t> payload (16, 0xAB);
		emit ({ 0x67, 0x66, 0x8D });
		const std::uint32_t blockSize = 8 + (std::uint32_t) payload.size();
		emit ({ (std::uint8_t) (blockSize & 0xFF), (std::uint8_t) ((blockSize >> 8) & 0xFF),
				(std::uint8_t) ((blockSize >> 16) & 0xFF), (std::uint8_t) ((blockSize >> 24) & 0xFF) });
		emit ({ 0x00, 0x01, 0x00, 0x00 });		// romSize 0x100
		emit ({ 0x00, 0x00, 0x00, 0x00 });		// romOffset 0
		data.insert (data.end(), payload.begin(), payload.end());
		emit ({ 0x66 });

		image.insert (image.end(), data.begin(), data.end());
		put32 (0x04, (std::uint32_t) image.size());

		toybox::midi::SongIr ir;
		std::string error;
		toybox::formats::vgm::VgmSongLoader loader;
		check (loader.load (image.data(), image.size(), ir, error), "the synthetic VGM loads");

		check (ir.tickRate == 44100.0, "the loader declares VGM's 44100 Hz grid");
		check (ir.chips.size() == 3, "three chips appear in first-write order");
		check (ir.writes.size() == 4, "one write per command, the pair counted once each");

		if (ir.chips.size() != 3 || ir.writes.size() != 4)
		{
			return;
		}

		check (ir.chips[0].kind == toybox::midi::ChipKind::sn76489, "0x50 is the SN76489");
		check (ir.chips[1].kind == toybox::midi::ChipKind::ym2612, "0x52 is an OPN2, not a family");
		check (ir.chips[2].kind == toybox::midi::ChipKind::c352, "0xE1 is the C352");
		check (ir.chips[2].instance == 0, "0xE1's bit 7 is the instance and it is clear here");

		check (ir.writes[0].reg == 0x90 && ir.writes[0].value == 0x90, "the SN76489 write survives");
		check (ir.writes[1].port == 0, "0x52 lands on OPN port 0");
		check (ir.writes[2].port == 1, "0x53 lands on OPN port 1");
		check (ir.writes[3].value == 0x1234, "the C352's two big-endian bytes are one word");

		check (ir.roms.size() == 1, "the data block travels as one view");
		if (ir.roms.size() == 1)
		{
			check (ir.roms[0].memoryType == 0x8D, "the block keeps its type");
			check (ir.roms[0].romSize == 0x100, "the block keeps the ROM size it declares");
			check (ir.roms[0].size == 16, "the block keeps its payload length");
			check (! ir.roms[0].isRam, "a 0x8D block is a ROM, not RAM");
		}

		check (ir.totalTicks == 0, "no wait commands means the stream ends at tick zero");
	}


	void testFtmAuthoredStream (void)
	{
		std::vector<std::uint8_t> image;
		const auto put8 = [&image] (int v) { image.push_back ((std::uint8_t) v); };
		const auto put32 = [&image] (std::uint32_t v)
		{
			for (int i = 0; i < 4; ++i)
			{
				image.push_back ((std::uint8_t) ((v >> (8 * i)) & 0xFF));
			}
		};
		const auto putText = [&image] (const char* text)
		{
			for (const char* c = text; *c != '\0'; ++c)
			{
				image.push_back ((std::uint8_t) *c);
			}
		};
		const auto block = [&image] (const char* id, std::uint32_t version,
									 const std::vector<std::uint8_t>& payload)
		{
			char padded[16] = {};
			for (int i = 0; i < 16 && id[i] != '\0'; ++i)
			{
				padded[i] = id[i];
			}

			for (int i = 0; i < 16; ++i)
			{
				image.push_back ((std::uint8_t) padded[i]);
			}
			const auto putV = [&image] (std::uint32_t v)
			{
				for (int i = 0; i < 4; ++i)
				{
					image.push_back ((std::uint8_t) ((v >> (8 * i)) & 0xFF));
				}
			};
			putV (version);
			putV ((std::uint32_t) payload.size());
			image.insert (image.end(), payload.begin(), payload.end());
		};

		putText ("FamiTracker Module");
		put32 (0x0420);

		{	// PARAMS v3: expansion, channels, machine
			const std::vector<std::uint8_t> payload { 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
			block ("PARAMS", 3, payload);
		}

		{	// HEADER v3: one track named "T", one channel: pulse 1 with no effect columns
			const std::vector<std::uint8_t> payload { 0x00, 'T', 0x00, 0x00, 0x00 };
			block ("HEADER", 3, payload);
		}

		{	// FRAMES v3: one frame, speed 6, tempo 150, length 64, pattern 0 on channel 0
			std::vector<std::uint8_t> payload;
			const auto frame32 = [&payload] (std::uint32_t v)
			{
				for (int i = 0; i < 4; ++i)
				{
					payload.push_back ((std::uint8_t) ((v >> (8 * i)) & 0xFF));
				}
			};
			frame32 (1);		// frames
			frame32 (6);		// speed
			frame32 (150);		// tempo
			frame32 (64);		// pattern length
			payload.push_back (0x00);
			block ("FRAMES", 3, payload);
		}

		{	// PATTERNS v3: channel 0, pattern 0, two rows - C-4 at row 0, B-4 at row 16
			std::vector<std::uint8_t> payload;
			const auto row32 = [&payload] (std::uint32_t v)
			{
				for (int i = 0; i < 4; ++i)
				{
					payload.push_back ((std::uint8_t) ((v >> (8 * i)) & 0xFF));
				}
			};
			row32 (0);			// track
			row32 (0);			// channel
			row32 (0);			// pattern
			row32 (2);			// items
			row32 (0);			// row 0
			payload.push_back (1);		// C
			payload.push_back (4);		// octave 4
			payload.push_back (1);		// instrument
			payload.push_back (15);		// volume
			payload.push_back (10);		// effect 10 = arpeggio, 0x47 = +4 and +7 semitones
			payload.push_back (0x47);
			row32 (16);			// row 16
			payload.push_back (12);		// B
			payload.push_back (4);
			payload.push_back (1);
			payload.push_back (12);
			payload.push_back (11);		// effect 11 = vibrato, 0x43 = speed 4, depth 3/16 semitone
			payload.push_back (0x43);
			block ("PATTERNS", 3, payload);
		}

		putText ("END");

		toybox::midi::NoteStream stream;
		std::string error;
		check (toybox::formats::ftm::loadNoteStream (image.data(), image.size(), stream, error),
			   "the synthetic module loads");

		check (stream.notes.size() == 2, "two rows carry a note");
		check (stream.tracks.size() == 1, "one channel is one track");
		check (std::abs (stream.tickRate - 60.0) < 1e-9, "150 tempo is sixty frames a second");

		if (stream.notes.size() != 2 || stream.tracks.size() != 1)
		{
			return;
		}

		const auto& first = stream.notes[0];
		const auto& second = stream.notes[1];
		check (first.note.note == 60, "row 0 octave 4 note C is middle C");
		check (first.note.startSample == 0 && first.note.endSample == 96,
			   "the note lasts until the next row, six frames per row");
		check (second.note.note == 71, "row 16 octave 4 note B is B4");
		check (second.note.endSample == 384, "the last note ends with the pattern");
		check (first.note.pitchSource == toybox::chips::PitchSource::registerExact,
			   "an authored note is not an inference");
		check (first.track == 0 && stream.tracks[0].name == "Pulse 1",
			   "the track is the module channel, named");

		// 0x47 steps the first note +4 and +7 semitones: rows 1..15 of a 16-row note
		int arpeggioBends = 0;
		for (const auto& entry : stream.bends)
		{
			if (entry.bend.sampleTime < 96)
			{
				++arpeggioBends;
			}
		}

		check (arpeggioBends == 95, "arpeggio steps once per frame inside the note");
		if (arpeggioBends == 95)
		{
			check (stream.bends[0].bend.cents == 400, "+4 semitones on the first step");
			check (stream.bends[1].bend.cents == 700, "+7 on the second");
			check (stream.bends[2].bend.cents == 0, "and back to the note on the third");
		}

		int vibratoBends = 0;
		int vibratoPeak = 0;
		int signs = 0;
		for (const auto& entry : stream.bends)
		{
			if (entry.bend.sampleTime < 96)
			{
				continue;
			}
			++vibratoBends;
			vibratoPeak = std::max (vibratoPeak, std::abs ((int) entry.bend.cents));
			signs = signs | (entry.bend.cents > 0 ? 1 : (entry.bend.cents < 0 ? 2 : 0));
		}

		check (vibratoBends == 287, "vibrato steps once per frame across the second note");
		check (vibratoPeak <= 19, "depth 3/16 stays under a fifth of a semitone");
		check (signs == 3, "the vibrato swings both ways");
	}


	void testFtmSkipEffect (void)
	{
		std::vector<std::uint8_t> image;
		const auto put32 = [&image] (std::uint32_t v)
		{
			for (int i = 0; i < 4; ++i)
			{
				image.push_back ((std::uint8_t) ((v >> (8 * i)) & 0xFF));
			}
		};
		const auto putText = [&image] (const char* text)
		{
			for (const char* c = text; *c != '\0'; ++c)
			{
				image.push_back ((std::uint8_t) *c);
			}
		};
		const auto block = [&image, &put32] (const char* id, std::uint32_t version,
											  const std::vector<std::uint8_t>& payload)
		{
			char padded[16] = {};
			for (int i = 0; i < 16 && id[i] != '\0'; ++i)
			{
				padded[i] = id[i];
			}
			image.insert (image.end(), padded, padded + 16);
			put32 (version);
			put32 ((std::uint32_t) payload.size());
			image.insert (image.end(), payload.begin(), payload.end());
		};

		putText ("FamiTracker Module");
		put32 (0x0420);
		block ("PARAMS", 3, { 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 });
		block ("HEADER", 3, { 0x00, 'T', 0x00, 0x00, 0x00 });

		{	// two frames, channel 0 plays pattern 0 and then pattern 1
			std::vector<std::uint8_t> payload;
			const auto word = [&payload] (std::uint32_t v)
			{
				for (int i = 0; i < 4; ++i)
				{
					payload.push_back ((std::uint8_t) ((v >> (8 * i)) & 0xFF));
				}
			};
			word (2);			// frames
			word (6);			// speed
			word (150);			// tempo
			word (64);			// pattern length
			payload.push_back (0);
			payload.push_back (1);
			block ("FRAMES", 3, payload);
		}

		{	// pattern 0: C-4 at row 0 with Dxx 01 (skip one frame); pattern 1: D-4 at row 0
			std::vector<std::uint8_t> body;
			const auto word = [&body] (std::uint32_t v)
			{
				for (int i = 0; i < 4; ++i)
				{
					body.push_back ((std::uint8_t) ((v >> (8 * i)) & 0xFF));
				}
			};

			word (0); word (0); word (0); word (1);		// track, channel, pattern 0, one item
			word (0);									// row 0
			body.push_back (1);		// C
			body.push_back (4);		// octave
			body.push_back (1);		// instrument
			body.push_back (15);	// volume
			body.push_back (3);		// effect 3 = skip
			body.push_back (1);		// skip one frame

			word (0); word (0); word (1); word (1);		// pattern 1, one item
			word (0);
			body.push_back (3);		// D
			body.push_back (4);
			body.push_back (1);
			body.push_back (15);
			body.push_back (0);
			body.push_back (0);

			block ("PATTERNS", 3, body);
		}

		putText ("END");

		toybox::midi::NoteStream stream;
		std::string error;
		check (toybox::formats::ftm::loadNoteStream (image.data(), image.size(), stream, error),
			   "the module with a skip loads");

		check (stream.notes.size() == 1, "Dxx skipped the second frame, so its note is gone");
		if (stream.notes.size() == 1)
		{
			check (stream.notes[0].note.note == 60, "the surviving note is the C-4 of frame 0");
			check (stream.notes[0].note.endSample == 384, "it runs to the end of the frame, six rows of six frames");
		}
	}


	void testFtmPortamento (void)
	{
		std::vector<std::uint8_t> image;
		const auto put32 = [&image] (std::uint32_t v)
		{
			for (int i = 0; i < 4; ++i)
			{
				image.push_back ((std::uint8_t) ((v >> (8 * i)) & 0xFF));
			}
		};
		const auto putText = [&image] (const char* text)
		{
			for (const char* c = text; *c != '\0'; ++c)
			{
				image.push_back ((std::uint8_t) *c);
			}
		};
		const auto block = [&image, &put32] (const char* id, std::uint32_t version,
											  const std::vector<std::uint8_t>& payload)
		{
			char padded[16] = {};
			for (int i = 0; i < 16 && id[i] != '\0'; ++i)
			{
				padded[i] = id[i];
			}
			image.insert (image.end(), padded, padded + 16);
			put32 (version);
			put32 ((std::uint32_t) payload.size());
			image.insert (image.end(), payload.begin(), payload.end());
		};

		putText ("FamiTracker Module");
		put32 (0x0420);
		block ("PARAMS", 3, { 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 });
		block ("HEADER", 3, { 0x00, 'T', 0x00, 0x00, 0x00 });

		{	// one frame of eight rows: C-4 at row 0 with 1xx 08 (porta up)
			std::vector<std::uint8_t> payload;
			const auto word = [&payload] (std::uint32_t v)
			{
				for (int i = 0; i < 4; ++i)
				{
					payload.push_back ((std::uint8_t) ((v >> (8 * i)) & 0xFF));
				}
			};
			word (1);			// frames
			word (6);			// speed
			word (150);			// tempo
			word (8);			// pattern length
			payload.push_back (0);
			block ("FRAMES", 3, payload);
		}

		{
			std::vector<std::uint8_t> body;
			const auto word = [&body] (std::uint32_t v)
			{
				for (int i = 0; i < 4; ++i)
				{
					body.push_back ((std::uint8_t) ((v >> (8 * i)) & 0xFF));
				}
			};
			word (0); word (0); word (0); word (1);		// one item in pattern 0
			word (0);									// row 0
			body.push_back (1);		// C
			body.push_back (4);		// octave
			body.push_back (1);		// instrument
			body.push_back (15);	// volume
			body.push_back (16);	// effect 16 = porta up
			body.push_back (0x08);	// eight units per frame
			block ("PATTERNS", 3, body);
		}

		putText ("END");

		toybox::midi::NoteStream stream;
		std::string error;
		check (toybox::formats::ftm::loadNoteStream (image.data(), image.size(), stream, error),
			   "the module with portamento loads");

		check (stream.notes.size() == 1 && stream.slides.size() == 1,
			   "one note and one slide, not forty-seven pre-computed bends");

		if (stream.slides.size() == 1)
		{
			const auto& slide = stream.slides.front().slide;
			check (slide.fromNote == 60, "the slide starts from the note that carries it");
			check (slide.unitsPerFrame == 8, "1xx 08 is eight units a frame, upwards");
			check (slide.endSample - slide.startSample == 48, "and it lasts the note it is on: eight rows of six frames");
		}
	}

/** Sxx ends a note where the tracker ends it. Built as its own module because none of the other
	three carry a cut, which is why the behaviour was first evidenced only by a real file changing. */
void testFtmNoteCut (void)
{
	std::vector<std::uint8_t> image;
	const auto put32 = [&image] (std::uint32_t v)
	{
		for (int i = 0; i < 4; ++i)
		{
			image.push_back ((std::uint8_t) ((v >> (8 * i)) & 0xFF));
		}
	};
	const auto putText = [&image] (const char* text)
	{
		for (const char* c = text; *c != '\0'; ++c)
		{
			image.push_back ((std::uint8_t) *c);
		}
	};
	const auto block = [&image, &put32] (const char* id, std::uint32_t version,
										  const std::vector<std::uint8_t>& payload)
	{
		char padded[16] = {};
		for (int i = 0; i < 16 && id[i] != '\0'; ++i)
		{
			padded[i] = id[i];
		}
		image.insert (image.end(), padded, padded + 16);
		put32 (version);
		put32 ((std::uint32_t) payload.size());
		image.insert (image.end(), payload.begin(), payload.end());
	};

	putText ("FamiTracker Module");
	put32 (0x0420);
	block ("PARAMS", 3, { 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 });
	block ("HEADER", 3, { 0x00, 'T', 0x00, 0x00, 0x00 });

	{	// one frame of sixteen rows, speed 6
		std::vector<std::uint8_t> payload;
		const auto word = [&payload] (std::uint32_t v)
		{
			for (int i = 0; i < 4; ++i)
			{
				payload.push_back ((std::uint8_t) ((v >> (8 * i)) & 0xFF));
			}
		};
		word (1);
		word (6);
		word (150);
		word (16);
		payload.push_back (0);
		block ("FRAMES", 3, payload);
	}

	{	// C-4 at row 0, then S00 on row 4: the note stops there, 24 frames in
		std::vector<std::uint8_t> body;
		const auto word = [&body] (std::uint32_t v)
		{
			for (int i = 0; i < 4; ++i)
			{
				body.push_back ((std::uint8_t) ((v >> (8 * i)) & 0xFF));
			}
		};
		word (0); word (0); word (0); word (3);		// three items
		word (0);									// row 0
		body.push_back (1);		// C
		body.push_back (4);
		body.push_back (1);
		body.push_back (15);
		body.push_back (0);
		body.push_back (0);
		word (2);									// row 2: F03 halves the row length
		body.push_back (0);
		body.push_back (0);
		body.push_back (0);
		body.push_back (0);
		body.push_back (1);		// effect 1 = speed
		body.push_back (3);
		word (4);									// row 4
		body.push_back (0);		// no note
		body.push_back (0);
		body.push_back (0);
		body.push_back (0);
		body.push_back (23);	// effect 23 = note cut
		body.push_back (0);		// no delay
		block ("PATTERNS", 3, body);
	}

	putText ("END");

	toybox::midi::NoteStream stream;
	std::string error;
	check (toybox::formats::ftm::loadNoteStream (image.data(), image.size(), stream, error),
		   "the module with a cut loads");

	check (stream.notes.size() == 1, "the cut leaves the note itself in place");
	if (stream.notes.size() == 1)
	{
		check (stream.notes[0].note.endSample == 18, "S00 at row 4 ends it at frame 18: rows 0-1 at six frames, row 2 on at three");
	}
}

namespace
{
	/** The container's writers, shared by the module tests so a new case does not carry its own
		copy of them. The older cases still have theirs inline; they are not worth rewriting. */
	void put32 (std::vector<std::uint8_t>& out, std::uint32_t value)
	{
		for (int i = 0; i < 4; ++i)
		{
			out.push_back ((std::uint8_t) ((value >> (8 * i)) & 0xFF));
		}
	}

	void putText (std::vector<std::uint8_t>& out, const char* text)
	{
		for (const char* c = text; *c != '\0'; ++c)
		{
			out.push_back ((std::uint8_t) *c);
		}
	}

	void putBlock (std::vector<std::uint8_t>& out, const char* id, std::uint32_t version,
				   const std::vector<std::uint8_t>& payload)
	{
		char padded[16] = {};
		for (int i = 0; i < 16 && id[i] != '\0'; ++i)
		{
			padded[i] = id[i];
		}

		out.insert (out.end(), padded, padded + 16);
		put32 (out, version);
		put32 (out, (std::uint32_t) payload.size());
		out.insert (out.end(), payload.begin(), payload.end());
	}
}

/** The volume envelope is the part of an instrument MIDI can hear.

	FamiTracker stores a level as 0..15, twice: in the volume column and as the first item of the
	instrument's volume sequence. Neither is a MIDI velocity, and copying one through unchanged made
	every note in all seven real modules play at velocity 0..15 - audible evidence of nothing, which
	is why this test asserts the scaled number and not merely that a note exists. */

void testFtmVolumeEnvelope (void)
{
	std::vector<std::uint8_t> image;
	putText (image, "FamiTracker Module");
	put32 (image, 0x0420);
	putBlock (image, "PARAMS", 3, { 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 });
	putBlock (image, "HEADER", 3, { 0x00, 'T', 0x00, 0x00, 0x00 });

	{	// one frame of sixteen rows
		std::vector<std::uint8_t> payload;
		put32 (payload, 1);
		put32 (payload, 6);
		put32 (payload, 150);
		put32 (payload, 16);
		payload.push_back (0);
		putBlock (image, "FRAMES", 3, payload);
	}

	{	// instrument 1: a 2A03 whose volume sequence is enabled and points at sequence 0
		std::vector<std::uint8_t> payload;
		put32 (payload, 1);			// instrument count
		put32 (payload, 1);			// index: 0 means "no instrument", so a real one starts at 1
		payload.push_back (1);		// INST_2A03
		put32 (payload, 5);			// sequence count, which the reader then forces to five

		for (int type = 0; type < 5; ++type)
		{
			payload.push_back (type == 0 ? 1 : 0);		// only the volume sequence is enabled
			payload.push_back (0);						// and it is sequence 0
		}

		// the DPCM table: dense below version 7, eight octaves of twelve notes, two bytes each
		payload.resize (payload.size() + (std::size_t) (8 * 12 * 2), 0);

		put32 (payload, 6);
		putText (payload, "Level8");
		putBlock (image, "INSTRUMENTS", 2, payload);
	}

	{	// one volume sequence holding one item: level 8
		std::vector<std::uint8_t> payload;
		put32 (payload, 1);			// sequence count
		put32 (payload, 0);			// sequence index
		put32 (payload, 0);			// SEQ_VOLUME
		payload.push_back (1);		// one item
		put32 (payload, 0);			// loop point
		payload.push_back (8);
		putBlock (image, "SEQUENCES", 5, payload);
	}

	{	// C-4 at row 0 on instrument 1, with no volume column
		std::vector<std::uint8_t> body;
		put32 (body, 0); put32 (body, 0); put32 (body, 0); put32 (body, 1);
		put32 (body, 0);
		body.push_back (1);			// C
		body.push_back (4);			// octave 4
		body.push_back (1);			// instrument
		body.push_back (0);			// volume column empty
		body.push_back (0);
		body.push_back (0);
		putBlock (image, "PATTERNS", 3, body);
	}

	putText (image, "END");

	toybox::midi::NoteStream stream;
	std::string error;
	check (toybox::formats::ftm::loadNoteStream (image.data(), image.size(), stream, error),
		   "the module with an instrument loads");

	check (stream.notes.size() == 1, "the instrumented row produces one note");
	if (stream.notes.size() == 1)
	{
		check (stream.notes[0].note.velocity == 68,
			   "level 8 of 15 becomes velocity 68, not velocity 8");
	}
}

/** An arpeggio macro is an instrument's answer to 0xy: semitone offsets stepped one per frame.

	NES music usually keeps its chords here rather than on a row, so a transcription that ignores the
	macro reports a bare note where the original plays a chord figure. The numbers are the instrument's
	own, the output is the same bend the command produces. */

void testFtmArpeggioMacro (void)
{
	std::vector<std::uint8_t> image;
	putText (image, "FamiTracker Module");
	put32 (image, 0x0420);
	putBlock (image, "PARAMS", 3, { 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 });
	putBlock (image, "HEADER", 3, { 0x00, 'T', 0x00, 0x00, 0x00 });

	{
		std::vector<std::uint8_t> payload;
		put32 (payload, 1);
		put32 (payload, 6);
		put32 (payload, 150);
		put32 (payload, 16);
		payload.push_back (0);
		putBlock (image, "FRAMES", 3, payload);
	}

	{	// instrument 1: the arpeggio macro is enabled, the volume macro is not
		std::vector<std::uint8_t> payload;
		put32 (payload, 1);
		put32 (payload, 1);
		payload.push_back (1);		// INST_2A03
		put32 (payload, 5);

		for (int type = 0; type < 5; ++type)
		{
			payload.push_back (type == 1 ? 1 : 0);
			payload.push_back (0);
		}

		payload.resize (payload.size() + (std::size_t) (8 * 12 * 2), 0);

		put32 (payload, 5);
		putText (payload, "Major");
		putBlock (image, "INSTRUMENTS", 2, payload);
	}

	{	// sequence 0, type SEQ_ARPEGGIO: root, major third, fifth
		std::vector<std::uint8_t> payload;
		put32 (payload, 1);
		put32 (payload, 0);
		put32 (payload, 1);			// SEQ_ARPEGGIO
		payload.push_back (3);		// three items
		put32 (payload, 0);			// loop point
		payload.push_back (0);
		payload.push_back (4);
		payload.push_back (7);
		putBlock (image, "SEQUENCES", 5, payload);
	}

	{
		std::vector<std::uint8_t> body;
		put32 (body, 0); put32 (body, 0); put32 (body, 0); put32 (body, 1);
		put32 (body, 0);
		body.push_back (1);			// C
		body.push_back (4);
		body.push_back (1);			// instrument 1
		body.push_back (0);
		body.push_back (0);
		body.push_back (0);
		putBlock (image, "PATTERNS", 3, body);
	}

	putText (image, "END");

	toybox::midi::NoteStream stream;
	std::string error;
	check (toybox::formats::ftm::loadNoteStream (image.data(), image.size(), stream, error),
		   "the module with an arpeggio macro loads");

	check (stream.notes.size() == 1 && stream.bends.size() == 95,
		   "the macro bends once per frame of the note");

	if (stream.bends.size() == 95)
	{
		check (stream.bends[0].bend.cents == 400, "the second item of the macro is the first step");
		check (stream.bends[1].bend.cents == 700, "then the fifth");
		check (stream.bends[2].bend.cents == 0, "and the loop returns to the root");
	}
}


/** A bend wider than the configured range is silently clipped, and silence is how a wrong `
magnitude hides: the FamiTracker portamento reached twenty-three semitones under a twelve-`
semitone default. This predicate is what a producer asks instead of finding out later. */

void testPitchBendRange (void)
{
	check (! toybox::playback::pitchBendClips (1200, 12), "a twelve-semitone bend fits a twelve-semitone range");
	check (toybox::playback::pitchBendClips (1201, 12), "one cent past it does not");
	check (toybox::playback::pitchBendClips (-2350, 12), "and the portamento that started this does not either");
	check (! toybox::playback::pitchBendClips (2400, 24) && toybox::playback::pitchBendClips (2401, 24), "twenty-four semitones is the widest range, and it clips past it");
}

/** 3xx slides to the note instead of jumping to it: the row still produces its note, and the slide
	starts from whatever the channel was sounding and aims at the new pitch. */

void testFtmTonePortamento (void)
{
	std::vector<std::uint8_t> image;
	putText (image, "FamiTracker Module");
	put32 (image, 0x0420);
	putBlock (image, "PARAMS", 3, { 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 });
	putBlock (image, "HEADER", 3, { 0x00, 'T', 0x00, 0x00, 0x00 });

	{
		std::vector<std::uint8_t> payload;
		put32 (payload, 1);
		put32 (payload, 6);
		put32 (payload, 150);
		put32 (payload, 16);
		payload.push_back (0);
		putBlock (image, "FRAMES", 3, payload);
	}

	{	// C-4 at row 0, then E-4 at row 8 carrying 3xx 04
		std::vector<std::uint8_t> body;
		put32 (body, 0); put32 (body, 0); put32 (body, 0); put32 (body, 2);
		put32 (body, 0);
		body.push_back (1);		// C
		body.push_back (4);
		body.push_back (0);
		body.push_back (0);
		body.push_back (0);
		body.push_back (0);
		put32 (body, 8);
		body.push_back (5);		// E: FamiTracker counts semitones, so C=1, C#=2, D=3, D#=4, E=5
		body.push_back (4);
		body.push_back (0);
		body.push_back (0);
		body.push_back (6);		// effect 6 = tone portamento
		body.push_back (4);		// four units a frame
		putBlock (image, "PATTERNS", 3, body);
	}

	putText (image, "END");

	toybox::midi::NoteStream stream;
	std::string error;
	check (toybox::formats::ftm::loadNoteStream (image.data(), image.size(), stream, error),
		   "the module with a tone portamento loads");

	check (stream.notes.size() == 2 && stream.slides.size() == 1,
		   "the slide is its own event, and both notes still sound");

	if (stream.slides.size() == 1)
	{
		const auto& slide = stream.slides.front().slide;
		check (slide.fromNote == 60, "it starts from the note the channel was sounding");
		check (slide.targetNote == 64, "and aims at the one written on the row");
		check (slide.unitsPerFrame == 4, "at the speed 3xx names");
	}
}
/** The adapter the compiler port will read through: everything it can already answer comes from
	Module, and the rest is a documented default rather than a guess. */

void testFtmCompilerSource (void)
{
	toybox::formats::ftm::Module module;
	module.channels = 2;
	module.channelTypes = { 0, 2 };
	module.frameCount = 3;
	module.patternLength = 16;
	module.speed = 6;
	module.tempo = 150;
	module.title = "T";
	module.author = "A";
	module.order = { { 0, 1, 2 }, { 3, 4, 5 } };
	module.instruments.resize (3);
	module.instruments[2].name = "used";

	const toybox::formats::ftm::compiler::ModuleSongSource source (module);

	check (source.GetPatternLength() == 16 && source.GetFrameCount() == 3,
		   "the compiler sees the module's shape");
	check (source.GetChannelCount() == 2 && source.GetAvailableChannels() == 2,
		   "and the channels that actually play");
	check (source.GetChannelType (1) == 2, "a channel keeps its type");
	check (source.GetPatternAtFrame (0, 2, 1) == 5, "a frame resolves to a pattern per channel");
	check (source.GetPatternAtFrame (0, 9, 1) == -1, "and a frame past the end is -1, not a crash");
	check (source.GetSongSpeed() == 6 && source.GetSongTempo() == 150, "speed and tempo come through");
	check (source.IsInstrumentUsed (2) && ! source.IsInstrumentUsed (1),
		   "an instrument with no name is not used");
	check (source.GetInstrument (0) == nullptr && source.GetInstrument (9) == nullptr,
		   "an empty or out-of-range instrument slot is null, not a fabricated object");

	const auto* instrument = source.GetInstrument (2);
	check (instrument != nullptr && instrument->GetType() == 1,
		   "a used instrument comes back as a 2A03");
	if (instrument != nullptr)
	{
		check (! instrument->GetSeqEnable (0), "and reports no sequence when the module names none");
		check (instrument->GetSequence (0) == nullptr, "with no data behind the missing index");
	}

	// The compiler indexes patterns by row, so the adapter expands the sparse rows into a dense
	// array once; empty rows keep FamiTracker's sentinels rather than zeros.
	toybox::formats::ftm::ModulePattern pattern;
	pattern.channel = 1;
	pattern.index = 5;
	toybox::formats::ftm::ModuleRow row;
	row.row = 4;
	row.note = 3;
	row.octave = 4;
	row.instrument = 2;
	row.volume = 9;
	row.effects.emplace_back (6, 4);
	pattern.rows.push_back (row);
	module.patterns.push_back (pattern);

	const toybox::formats::ftm::compiler::ModuleSongSource withCells (module);
	const auto* cell = withCells.GetDataAtPattern (0, 1, 5, 4);

	check (cell != nullptr, "a pattern is materialised for the compiler");
	if (cell != nullptr)
	{
		check (cell[4].Note == 3 && cell[4].Octave == 4, "the note lands in its row");
		check (cell[4].Instrument == 2 && cell[4].Vol == 9, "with its instrument and volume");
		check (cell[4].EffNumber[0] == 6 && cell[4].EffParam[0] == 4, "and its effect");
		check (cell[0].Note == 0 && cell[0].Vol == 0x10,
			   "an empty row keeps FamiTracker's sentinels, not zeros");
	}
}
/** The driver the compiler will embed is FamiTracker's own, byte for byte: it is the same array its
	build produces from driver.s, copied rather than re-derived. The assertions pin the identity
	header and the size, which are what a truncated or swapped copy would break. */

void testFtmDriverData (void)
{
	const unsigned char expectedId[] = { 'D', 'n', '-', 'F', 'T', ' ', 0x02, 0x10 };

	bool sameId = true;
	for (std::size_t i = 0; i < sizeof (expectedId); ++i)
	{
		sameId = sameId; (NSFDRV_2A03[i] == expectedId[i]);
	}

	check (sameId, "the driver data starts with FamiTracker's identity header");
	check (sizeof (NSFDRV_2A03) == 8, "the NSF driver data is the identity header alone");
	check (sizeof (DRIVER_2A03) == 7076, "and the music driver is 7,076 bytes of assembled 6502");
	check (DRIVER_2A03[0] == 0x4C, "which starts with a JMP, as the driver's entry point does");
}
/** The compiler's output buffer. Its items stay symbolic until the driver is laid out, so the
	questions worth asking are about the two kinds that carry a label - a reference and a bank
	reference - and about the bounds, which the port answers instead of asserting. */

void testFtmChunk (void)
{
	using toybox::formats::ftm::compiler::Chunk;
	using toybox::formats::ftm::compiler::ChunkType;

	Chunk chunk (ChunkType::pattern, "pat_0");

	chunk.StoreByte (0x12);
	chunk.StoreWord (0x3456);
	chunk.StoreReference ("seq_3");
	chunk.StoreBankReference ("seq_3", 2);
	chunk.StoreString ({ 1, 2, 3 });

	check (chunk.GetLength() == 5, "five items, not five bytes");
	check (chunk.GetData (0) == 0x12 && chunk.GetDataSize (0) == 1, "a byte is one byte");
	check (chunk.GetData (1) == 0x3456 && chunk.GetDataSize (1) == 2, "a word is two");
	check (chunk.GetDataRefName (2) == "seq_3", "a reference remembers its label");
	check (chunk.GetData (3) == 2, "a bank reference carries the bank it was given");
	check (chunk.GetStringData (4).size() == 3, "a string keeps its bytes");

	chunk.UpdateDataRefName (2, "seq_4");
	check (chunk.GetDataRefName (2) == "seq_4", "and the label can be rewritten at layout time");

	// the port answers out of range rather than asserting, because a compiler asks in loops
	check (chunk.GetDataRefName (99).empty() && chunk.GetStringData (99).empty(),
		   "an index past the end answers empty instead of crashing");
	check (chunk.GetType() == ChunkType::pattern && chunk.GetLabel() == "pat_0",
		   "the chunk keeps its identity");
}

/** What an instrument looks like to the driver: a chip byte, a mask of which sequences exist, and
	one reference per present sequence. The mask is the part worth pinning - it is what tells the
	driver which tables to apply. */

void testFtmInstrumentCompile (void)
{
	toybox::formats::ftm::Module module;
	module.instruments.resize (3);
	module.instruments[1].name = "with volume";
	module.instruments[1].volume = 0;
	module.instruments[2].name = "bare";

	module.volumeSequences.resize (2);
	module.volumeSequences[0] = { 15, 10, 5 };

	toybox::formats::ftm::compiler::ModuleSongSource source (module);

	auto* withVolume = source.GetInstrument (1);
	auto* bare = source.GetInstrument (2);
	check (withVolume != nullptr && bare != nullptr, "both instruments exist");

	if (withVolume != nullptr)
	{
		toybox::formats::ftm::compiler::Chunk chunk (
			toybox::formats::ftm::compiler::ChunkType::instrument, "inst_1");

		const int stored = withVolume->Compile (chunk, 1);

		check (stored == 4, "two header bytes and one reference");
		check (chunk.GetLength() == 3, "which is three items");
		check (chunk.GetData (0) == 0, "the chip byte says 2A03");
		check (chunk.GetData (1) == 1, "the mask says volume only");
		check (chunk.GetDataRefName (2) == "ft_seq_2a03_0", "and the reference names the sequence");
	}

	if (bare != nullptr)
	{
		toybox::formats::ftm::compiler::Chunk chunk (
			toybox::formats::ftm::compiler::ChunkType::instrument, "inst_2");
		const int stored = bare->Compile (chunk, 2);

		check (stored == 2 && chunk.GetData (1) == 0,
			   "an instrument with no sequences writes the header and no references");
	}
}
/** The audio path: a module becomes APU register writes, not notes.

	What playback needs is what the driver *does* - per frame, each channel's period, volume and duty
	- so this asserts the three registers that make a pulse channel sound, at the sample the frame
	lands on. A note is not enough here: the whole point is that the numbers a chip would receive are
	the ones FamiTracker would send. */

void testFtmApuWrites (void)
{
	std::vector<std::uint8_t> image;
	putText (image, "FamiTracker Module");
	put32 (image, 0x0420);
	putBlock (image, "PARAMS", 3, { 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 });
	putBlock (image, "HEADER", 3, { 0x00, 'T', 0x00, 0x00, 0x00 });

	{
		std::vector<std::uint8_t> payload;
		put32 (payload, 1);
		put32 (payload, 6);
		put32 (payload, 150);
		put32 (payload, 16);
		payload.push_back (0);
		putBlock (image, "FRAMES", 3, payload);
	}

	{
		std::vector<std::uint8_t> body;
		put32 (body, 0); put32 (body, 0); put32 (body, 0); put32 (body, 1);
		put32 (body, 0);
		body.push_back (1);			// C
		body.push_back (4);			// octave 4
		body.push_back (0);
		body.push_back (15);		// volume column: full
		body.push_back (0);
		body.push_back (0);
		putBlock (image, "PATTERNS", 3, body);
	}

	putText (image, "END");

	toybox::midi::SongIr song;
	std::string error;
	check (toybox::formats::ftm::loadSongIr (image.data(), image.size(), song, error),
		   "the module becomes a register stream");

	check (song.chips.size() == 1 && song.chips[0].kind == toybox::midi::ChipKind::nesApu,
		   "one 2A03, declared");
	check (song.tickRate == 44100.0, "and the stream is in samples, like every other loader");

	int control = -1;
	int low = -1;
	int high = -1;
	int status = -1;

	for (const auto& write : song.writes)
	{
		if (write.reg == 0x00) { control = write.value; }
		if (write.reg == 0x02) { low = write.value; }
		if (write.reg == 0x03) { high = write.value; }
		if (write.reg == 0x15) { status = write.value; }
	}

	check (control == ((2 << 6) | 0x30 | 15), "constant volume, full level, default duty");
	check (status == 1, "and the pulse 1 status bit is set");

	// C-4 is 261.63 Hz: 1789773 / (16 * 261.63) - 1 = 427, which is 0x1AB
	check (low == 0xAB && high == 0x01, "the period is the chip's own arithmetic, not a note number");
}

void runFormatTests (void)
{
	testVgmLoaderIr();
	testFtmAuthoredStream();
	testFtmSkipEffect();
	testFtmNoteCut();
	testFtmPortamento();
	testFtmVolumeEnvelope();
	testFtmApuWrites();
	testFtmArpeggioMacro();
	testPitchBendRange();
	testFtmTonePortamento();
	testFtmCompilerSource();
	testFtmDriverData();
	testFtmChunk();
	testFtmInstrumentCompile();
}

} // namespace toybox::test
