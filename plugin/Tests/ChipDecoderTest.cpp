// Chip-level tests: register writes in, notes out - no libvgm, no JUCE, no audio device.
//
// This is the layer the rest of the MIDI path trusts: if the 2A03 decoder says A4 lasts one
// second, the encoder will emit exactly that. Nothing here listens to a chip; every expectation
// is a number that follows from the register values.
//
//   cmake --build <build dir> --target vgm_chip_test
//   <build dir>/plugin/vgm_chip_test

#include <cstdint>
#include <cstdio>
#include <vector>

#include "chips/NesApuDecoder.h"
#include "chips/NoteExtractor.h"

namespace
{
	using toybox::chips::NoteEvent;
	using toybox::chips::RegisterWrite;

	int failures = 0;

	void check (bool ok, const char* what)
	{
		std::printf (" %s  %s\n", ok ? "OK  " : "FAIL", what);
		if (! ok)
		{
			++failures;
		}
	}

	/** Keeps the notes easy to read: `stream.at (44100, 0x03, 0x00)` is a write at one second. */
	struct Stream
	{
		std::vector<RegisterWrite> writes;

		void at (std::uint32_t time, std::uint8_t reg, std::uint8_t value)
		{
			RegisterWrite write;
			write.reg = reg;
			write.value = value;
			write.sampleTime = time;
			writes.push_back (write);
		}
	};

	std::vector<NoteEvent> run (const Stream& stream, std::uint32_t totalSamples)
	{
		toybox::chips::NesApuDecoder decoder;
		decoder.reset (1789773, 44100.0);
		return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
	}

	std::uint32_t distance (std::uint32_t a, std::uint32_t b)
	{
		return a > b ? a - b : b - a;
	}

	void enableApu (Stream& stream, std::uint32_t time = 0)
	{
		stream.at (time, 0x15, 0x0F);		// pulse 1/2, triangle, noise
	}

	/** Pulse 1, constant volume 15, period 252 = 442 Hz = A4 (69). */
	void startA4 (Stream& stream, std::uint32_t time)
	{
		stream.at (time, 0x00, 0x9F);		// duty 2, halt, constant volume 15
		stream.at (time, 0x02, 0xFC);
		stream.at (time, 0x03, 0x08);		// length index 1, timer high 0
	}

	void testPulseNote (void)
	{
		Stream stream;
		enableApu (stream);
		startA4 (stream, 0);
		stream.at (44100, 0x15, 0x00);		// clearing the enable bit keys the channel off

		const auto notes = run (stream, 88200);

		check (notes.size() == 1, "one pulse note in, one note out");
		if (notes.empty())
		{
			return;
		}

		check (notes[0].note == 69, "442 Hz decodes to A4");
		check (notes[0].startSample == 0, "the note starts with the write that opened it");
		check (distance (notes[0].endSample, 44100) <= 64, "the note ends where the length went to 0");
		check (notes[0].velocity == 127, "constant volume 15 is full velocity");
		check (notes[0].logicalChannel == 0, "pulse 1 is channel 0");
	}

	void testPitchChange (void)
	{
		Stream stream;
		enableApu (stream);
		startA4 (stream, 0);
		stream.at (22050, 0x02, 0xFB);		// period 507 = 220 Hz
		stream.at (22050, 0x03, 0x09);

		const auto notes = run (stream, 44100);

		check (notes.size() == 2, "a pitch change starts a second note");
		if (notes.size() != 2)
		{
			return;
		}

		check (notes[0].note == 69 && notes[1].note == 57, "the second note is an octave lower");
		check (notes[0].endSample == notes[1].startSample, "the notes meet without a gap");
		check (distance (notes[1].startSample, 22050) <= 64, "the second note starts at the write");
	}

	void testLengthCounterEndsTheNote (void)
	{
		Stream stream;
		enableApu (stream);
		stream.at (0, 0x00, 0x1F);			// halt off, constant volume 15
		stream.at (0, 0x02, 0xFC);
		stream.at (0, 0x03, 0x08);			// length index 1 = 254 half frames ~ 2.1 s

		const auto notes = run (stream, 132300);	// three seconds of stream, no explicit key off

		check (notes.size() == 1, "nothing retriggers the note");
		if (notes.empty())
		{
			return;
		}

		// 254 half frames * 7457 cycles / 1789773 Hz * 44100 = 93310 samples
		check (distance (notes[0].endSample, 93310) <= 256,
			   "the length counter ends the note by itself");
	}

	void testTriangle (void)
	{
		Stream stream;
		enableApu (stream);
		stream.at (0, 0x08, 0xFF);			// control on, linear reload 127
		stream.at (0, 0x0A, 0xFC);
		stream.at (0, 0x0B, 0x08);

		const auto notes = run (stream, 44100);

		check (notes.size() == 1, "the triangle holds one note");
		if (notes.empty())
		{
			return;
		}

		// 1789773 / (32 * 253) = 221 Hz
		check (notes[0].note == 57, "the triangle plays A3");
		check (notes[0].logicalChannel == 2, "the triangle is channel 2");
		check (notes[0].isDrum == false, "the triangle is a melodic channel");
	}

	void testNoiseIsADrum (void)
	{
		Stream stream;
		enableApu (stream);
		stream.at (0, 0x0C, 0x9F);			// halt off, constant volume 15
		stream.at (0, 0x0E, 0x04);			// period index 4
		stream.at (0, 0x0F, 0x08);

		const auto notes = run (stream, 22050);

		check (notes.size() == 1, "one noise hit");
		if (notes.empty())
		{
			return;
		}

		check (notes[0].isDrum, "the noise channel is a drum track");
		check (notes[0].note == 40, "period index 4 maps to the snare key");
		check (notes[0].logicalChannel == 3, "the noise is channel 3");
	}

	void testSecondPulseIsASeparateChannel (void)
	{
		Stream stream;
		enableApu (stream);
		startA4 (stream, 0);
		stream.at (0, 0x04, 0x9F);			// pulse 2, same pitch
		stream.at (0, 0x06, 0xFC);
		stream.at (0, 0x07, 0x08);

		const auto notes = run (stream, 22050);

		check (notes.size() == 2, "two pulses sound at once");
		check (notes.size() == 2 && notes[0].logicalChannel == 0
				   && notes[1].logicalChannel == 1,
			   "pulse 2 is its own channel");
	}

	void testSilenceProducesNoNotes (void)
	{
		Stream stream;
		enableApu (stream);
		stream.at (0, 0x00, 0x90);			// constant volume 0

		const auto notes = run (stream, 22050);

		check (notes.empty(), "a channel at volume 0 is not a note");
	}
}

int main (void)
{
	std::printf ("chip decoder test\n");

	testPulseNote();
	testPitchChange();
	testLengthCounterEndsTheNote();
	testTriangle();
	testNoiseIsADrum();
	testSecondPulseIsASeparateChannel();
	testSilenceProducesNoNotes();

	std::printf ("Result    : %s\n", failures == 0 ? "PASS" : "FAILURES");
	return failures == 0 ? 0 : 1;
}
