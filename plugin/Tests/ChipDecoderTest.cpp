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

#include "chips/GbcApuDecoder.h"
#include "chips/NesApuDecoder.h"
#include "chips/NoteExtractor.h"
#include "chips/Ym2612Decoder.h"

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

		void at (std::uint32_t time, std::uint8_t port, std::uint8_t reg, std::uint8_t value)
		{
			RegisterWrite write;
			write.port = port;
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

	void testNesVibratoBecomesAPitchBend (void)
	{
		Stream stream;
		enableApu (stream);
		startA4 (stream, 0);
		stream.at (22050, 0x02, 0xFB);		// period 251: ~8 cents sharp

		toybox::chips::NesApuDecoder decoder;
		decoder.reset (1789773, 44100.0);
		const auto performance = toybox::chips::extractPerformance (decoder, stream.writes, 33075);

		check (performance.notes.size() == 1, "a vibrato does not re-trigger the note");
		check (performance.bends.size() >= 2, "a vibrato arrives as a pitch bend");
		if (performance.bends.size() >= 2)
		{
			check (performance.bends.back().cents >= 5, "the bend carries the pitch difference in cents");
		}
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

	// --- YM2612 -------------------------------------------------------------------------------

	constexpr std::uint32_t kYmClock = 7670453;		// the Mega Drive's master clock

	template <typename Decoder>
	std::vector<NoteEvent> runChip (const Stream& stream, std::uint32_t totalSamples,
									std::uint32_t clock)
	{
		Decoder decoder;
		decoder.reset (clock, 44100.0);
		return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
	}

	/** Channel 1 at A4: fnum 0x21D, block 4 on a 7.67 MHz chip is 439.7 Hz. */
	void startFmNote (Stream& stream, std::uint32_t time, std::uint8_t algorithm,
					  std::uint8_t op1Level, std::uint8_t op4Level)
	{
		stream.at (time, 0x40, op1Level);		// operator 1 total level
		stream.at (time, 0x4C, op4Level);		// operator 4 total level
		stream.at (time, 0xA0, 0x1D);
		stream.at (time, 0xA4, 0x22);			// block 4, fnum high 2
		stream.at (time, 0xB0, algorithm);
		stream.at (time, 0x28, 0xF0);			// key on channel 1, all four operators
	}

	void testFmNote (void)
	{
		Stream stream;
		startFmNote (stream, 0, 0x00, 0x7F, 0x00);
		stream.at (44100, 0x28, 0x00);			// key off channel 1

		const auto notes = runChip<toybox::chips::Ym2612Decoder> (stream, 88200, kYmClock);

		check (notes.size() == 1, "one FM key-on is one note");
		if (notes.empty())
		{
			return;
		}

		check (notes[0].note == 69, "fnum 0x21D at block 4 decodes to A4");
		check (notes[0].startSample == 0, "the note starts with the key-on");
		check (distance (notes[0].endSample, 44100) <= 64, "key off ends the note");
		check (notes[0].logicalChannel == 0, "the first FM channel is channel 0");
		check (notes[0].velocity == 127, "a carrier at total level 0 is full velocity");
	}

	void testFmSilentCarrier (void)
	{
		Stream stream;
		startFmNote (stream, 0, 0x00, 0x00, 0x7F);	// the only carrier is silenced

		const auto notes = runChip<toybox::chips::Ym2612Decoder> (stream, 44100, kYmClock);

		check (notes.empty(), "a patch whose carrier is at maximum attenuation is not a note");
	}

	void testFmAlgorithmDecidesTheCarrier (void)
	{
		// Same total levels in both streams: operator 1 audible, operator 4 silenced. Algorithm 0
		// routes only operator 4 to the output, algorithm 7 sums all four.
		Stream silent;
		startFmNote (silent, 0, 0x00, 0x00, 0x7F);
		Stream audible;
		startFmNote (audible, 0, 0x07, 0x00, 0x7F);

		check (runChip<toybox::chips::Ym2612Decoder> (silent, 44100, kYmClock).empty(),
			   "algorithm 0 ignores the silent operator 4");
		check (runChip<toybox::chips::Ym2612Decoder> (audible, 44100, kYmClock).size() == 1,
			   "algorithm 7 outputs operator 1");
	}

	void testFmBlockIsAnOctave (void)
	{
		Stream stream;
		startFmNote (stream, 0, 0x00, 0x7F, 0x00);
		stream.at (22050, 0xA4, 0x2A);			// block 5, same f-number

		const auto notes = runChip<toybox::chips::Ym2612Decoder> (stream, 44100, kYmClock);

		check (notes.size() == 2, "a block change starts a second note");
		if (notes.size() == 2)
		{
			check (notes[1].note == 81, "one block up is one octave up");
		}
	}

	/** Vibrato and slides rewrite the f-number without re-keying: the note stays, its pitch moves. */
	void testFmPitchBendFollowsTheFnumber (void)
	{
		Stream stream;
		startFmNote (stream, 0, 0x00, 0x7F, 0x00);
		stream.at (22050, 0xA0, 0x20);			// fnum 0x21D -> 0x220, about 10 cents up
		stream.at (44100, 0x28, 0x00);

		toybox::chips::Ym2612Decoder decoder;
		decoder.reset (kYmClock, 44100.0);
		const auto performance = toybox::chips::extractPerformance (decoder, stream.writes, 66150);

		check (performance.notes.size() == 1, "a small f-number change does not re-trigger the note");
		check (performance.bends.size() >= 2, "the pitch change arrives as a bend");
		if (! performance.bends.empty())
		{
			const int cents = performance.bends.back().cents;
			check (std::abs ((int) performance.bends.front().cents) <= 2,
				   "a note starts on its own pitch, not on the previous note's bend");
			check (cents >= 5 && cents <= 20, "the bend follows the f-number in cents");
		}
	}

	void testFmSecondPortIsASeparateChannel (void)
	{
		Stream stream;
		startFmNote (stream, 0, 0x00, 0x7F, 0x00);	// channel 1
		stream.at (0, 1, 0x4C, 0x00);				// channel 4, operator 4
		stream.at (0, 1, 0xA0, 0x1D);
		stream.at (0, 1, 0xA4, 0x22);
		stream.at (0, 1, 0xB0, 0x00);
		stream.at (0, 0x28, 0xF4);					// key on channel 4, port select bit

		const auto notes = runChip<toybox::chips::Ym2612Decoder> (stream, 22050, kYmClock);

		check (notes.size() == 2, "both ports sound at once");
		check (notes.size() == 2 && notes[0].logicalChannel == 0 && notes[1].logicalChannel == 3,
			   "port 1 channels are their own tracks");
	}

	void testFmDacTakesOverChannelSix (void)
	{
		Stream stream;
		stream.at (0, 1, 0x4E, 0x00);				// channel 6, operator 4 total level
		stream.at (0, 1, 0xA2, 0x1D);
		stream.at (0, 1, 0xA6, 0x22);
		stream.at (0, 1, 0xB2, 0x00);
		stream.at (0, 0x28, 0xF6);					// key on channel 6

		const auto withoutDac = runChip<toybox::chips::Ym2612Decoder> (stream, 22050, kYmClock);

		stream.at (0, 0x2B, 0x80);					// DAC enable
		const auto withDac = runChip<toybox::chips::Ym2612Decoder> (stream, 22050, kYmClock);

		check (withoutDac.size() == 1 && withoutDac[0].logicalChannel == 5,
			   "channel 6 is an FM channel while the DAC is off");
		check (withDac.empty(), "the DAC replaces channel 6 instead of adding notes");
	}

	// --- Game Boy DMG -------------------------------------------------------------------------

	constexpr std::uint32_t kGbClock = 4194304;		// the DMG's CPU clock

	std::vector<NoteEvent> runGb (const Stream& stream, std::uint32_t totalSamples)
	{
		return runChip<toybox::chips::GbcApuDecoder> (stream, totalSamples, kGbClock);
	}

	/** Pulse 1 at A4: 131072 / (2048 - 1750) = 439.8 Hz. The envelope holds at 15, so only the
		power switch or the length counter can end the note. */
	void startGbA4 (Stream& stream, std::uint32_t time, std::uint8_t envelope = 0xF0,
					bool lengthEnable = false, std::uint8_t lengthLoad = 0)
	{
		// the trigger seeds the counter from the length load, so that write goes first
		stream.at (time, 0x01, (std::uint8_t) (0x80 | (lengthLoad & 0x3F)));	// duty 2
		stream.at (time, 0x02, envelope);	// NR12
		stream.at (time, 0x03, 0xD6);		// frequency 1750 low
		stream.at (time, 0x04, (std::uint8_t) (0x86 | (lengthEnable ? 0x40 : 0)));
	}

	void testGbPulseNote (void)
	{
		Stream stream;
		stream.at (0, 0x16, 0x80);			// NR52: sound on
		startGbA4 (stream, 0);
		stream.at (44100, 0x16, 0x00);		// power off

		const auto notes = runGb (stream, 88200);

		check (notes.size() == 1, "one Game Boy pulse note in, one note out");
		if (notes.empty())
		{
			return;
		}

		check (notes[0].note == 69, "frequency 1750 decodes to A4");
		check (notes[0].startSample == 0, "the note starts with the trigger");
		check (distance (notes[0].endSample, 44100) <= 128, "the power switch ends the note");
		check (notes[0].velocity == 127, "envelope volume 15 is full velocity");
		check (notes[0].logicalChannel == 0, "pulse 1 is channel 0");
		check (notes[0].family == toybox::chips::Family::gbPulse, "the note carries the GB family");
	}

	void testGbLengthCounterEndsTheNote (void)
	{
		Stream stream;
		stream.at (0, 0x16, 0x80);
		startGbA4 (stream, 0, 0xF0, true, 48);	// length load 48: the counter starts at 16

		const auto notes = runGb (stream, 44100);

		check (notes.size() == 1, "nothing but the length counter ends the note");
		if (notes.empty())
		{
			return;
		}

		// 16 length clocks at 256 Hz, the first one 86 samples in: 86 + 15 * 172.3 = 2670
		check (distance (notes[0].endSample, 2670) <= 200,
			   "the length counter ends the note by itself");
	}

	void testGbEnvelopeDecaysTheNoteAway (void)
	{
		Stream stream;
		stream.at (0, 0x16, 0x80);
		startGbA4 (stream, 0, 0x81);		// initial volume 8, decrease, period 1

		const auto notes = runGb (stream, 44100);

		check (notes.size() == 1, "the envelope, not the driver, ends this note");
		if (notes.empty())
		{
			return;
		}

		// 8 volume steps, two 64 Hz clocks each, first envelope clock 689 samples in
		check (distance (notes[0].endSample, 11024) <= 300,
			   "the envelope runs down at 64 Hz and silences the channel");
		check (notes[0].velocity == 68, "the note keeps the volume it was triggered at");
	}

	void testGbWaveChannel (void)
	{
		Stream stream;
		stream.at (0, 0x16, 0x80);
		stream.at (0, 0x0A, 0x80);			// NR30: wave DAC on
		stream.at (0, 0x0C, 0x20);			// NR32: full volume
		stream.at (0, 0x0D, 0x6B);			// 65536 / (2048 - 1899) = 439.8 Hz
		stream.at (0, 0x0E, 0x87);			// trigger, frequency high 7

		const auto notes = runGb (stream, 22050);

		check (notes.size() == 1, "the wave channel is a tone channel");
		if (notes.empty())
		{
			return;
		}

		check (notes[0].note == 69, "the wave channel divides the clock by 64, so 1899 is A4");
		check (notes[0].logicalChannel == 2, "the wave channel is channel 2");
		check (notes[0].family == toybox::chips::Family::gbWave, "the note carries the wave family");
		check (notes[0].isDrum == false, "the wave channel is melodic");
	}

	void testGbNoiseIsADrum (void)
	{
		Stream fast;
		fast.at (0, 0x16, 0x80);
		fast.at (0, 0x11, 0xF0);			// NR42: volume 15
		fast.at (0, 0x12, 0x00);			// NR43: shift 0, divisor 8 - the highest frequency
		fast.at (0, 0x13, 0x80);			// trigger

		Stream slow;
		slow.at (0, 0x16, 0x80);
		slow.at (0, 0x11, 0xF0);
		slow.at (0, 0x12, 0x80);			// shift 8: 256 times lower
		slow.at (0, 0x13, 0x80);

		const auto fastNotes = runGb (fast, 22050);
		const auto slowNotes = runGb (slow, 22050);

		check (fastNotes.size() == 1 && slowNotes.size() == 1, "one noise hit each");
		if (fastNotes.empty() || slowNotes.empty())
		{
			return;
		}

		check (fastNotes[0].isDrum, "the noise channel is a drum track");
		check (fastNotes[0].logicalChannel == 3, "the noise is channel 3");
		check (fastNotes[0].family == toybox::chips::Family::gbNoise, "the note carries the noise family");
		check (fastNotes[0].note == 38, "the fastest noise lands on the first drum key");
		check (slowNotes[0].note > fastNotes[0].note, "a slower noise lands on a higher drum key");
	}

	void testGbSilenceWithoutPower (void)
	{
		Stream stream;
		startGbA4 (stream, 0);				// never turns NR52 on

		check (runGb (stream, 22050).empty(), "an APU that was never powered on makes no notes");
	}
}

int main (void)
{
	std::printf ("chip decoder test\n");

	testPulseNote();
	testPitchChange();
	testNesVibratoBecomesAPitchBend();
	testLengthCounterEndsTheNote();
	testTriangle();
	testNoiseIsADrum();
	testSecondPulseIsASeparateChannel();
	testSilenceProducesNoNotes();

	testFmNote();
	testFmSilentCarrier();
	testFmAlgorithmDecidesTheCarrier();
	testFmBlockIsAnOctave();
	testFmPitchBendFollowsTheFnumber();
	testFmSecondPortIsASeparateChannel();
	testFmDacTakesOverChannelSix();

	testGbPulseNote();
	testGbLengthCounterEndsTheNote();
	testGbEnvelopeDecaysTheNoteAway();
	testGbWaveChannel();
	testGbNoiseIsADrum();
	testGbSilenceWithoutPower();

	std::printf ("Result    : %s\n", failures == 0 ? "PASS" : "FAILURES");
	return failures == 0 ? 0 : 1;
}
