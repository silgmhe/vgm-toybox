#include "FormatsTests.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../Support/TestSupport.h"
#include "chips/ChipTypes.h"
#include "formats/ftm/FtmSongLoader.h"
#include "formats/vgm/VgmSongLoader.h"
#include "midi/NoteStream.h"

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

		check (stream.notes.size() == 1 && stream.bends.size() == 47,
			   "forty-seven frames after the first step once each");

		if (stream.bends.size() == 47)
		{
			check (stream.bends.front().bend.cents == 50, "eight units of 1/16 semitone is 50 cents");
			check (stream.bends.back().bend.cents == 2350, "and it keeps climbing, one step per frame");

			bool monotonic = true;
			for (std::size_t i = 1; i < stream.bends.size(); ++i)
			{
				monotonic = monotonic && stream.bends[i].bend.cents > stream.bends[i - 1].bend.cents;
			}
			check (monotonic, "porta up never steps back down");
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
		word (0); word (0); word (0); word (2);		// two items
		word (0);									// row 0
		body.push_back (1);		// C
		body.push_back (4);
		body.push_back (1);
		body.push_back (15);
		body.push_back (0);
		body.push_back (0);
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
		check (stream.notes[0].note.endSample == 24, "S00 on row 4 ends it at frame 24");
	}
}

void runFormatTests (void)
{
	testVgmLoaderIr();
	testFtmAuthoredStream();
	testFtmSkipEffect();
	testFtmNoteCut();
	testFtmPortamento();
}

} // namespace toybox::test
