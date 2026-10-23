// Chip-level tests: register writes in, notes out - no libvgm, no JUCE, no audio device.
//
// This is the layer the rest of the MIDI path trusts: if the 2A03 decoder says A4 lasts one
// second, the encoder will emit exactly that. Nothing here listens to a chip; every expectation
// is a number that follows from the register values.
//
//   cmake --build <build dir> --target vgm_chip_test
//   <build dir>/plugin/vgm_chip_test

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "chips/Ay8910Decoder.h"
#include "chips/C140Decoder.h"
#include "chips/C352Decoder.h"
#include "chips/GbcApuDecoder.h"
#include "chips/HuC6280Decoder.h"
#include "chips/K051649Decoder.h"
#include "chips/K053260Decoder.h"
#include "chips/K054539Decoder.h"
#include "chips/QSoundDecoder.h"
#include "chips/Es5503Decoder.h"
#include "chips/MikeyDecoder.h"
#include <fstream>
#include <iterator>
#include <string>
#include "chips/Pitch.h"
#include "formats/ftm/FtmSongLoader.h"
#include "formats/vgm/VgmSongLoader.h"
#include "chips/Msm5232Decoder.h"
#include "chips/K005289Decoder.h"
#include "chips/VsuDecoder.h"
#include "chips/Okim6295Decoder.h"
#include "chips/Ga20Decoder.h"
#include "chips/SegaPcmDecoder.h"
#include "chips/NesApuDecoder.h"
#include "chips/NoteExtractor.h"
#include "chips/PokeyDecoder.h"
#include "chips/Rf5c68Decoder.h"
#include "chips/X1_010Decoder.h"
#include "chips/Ymf271Decoder.h"
#include "chips/Ymz280bDecoder.h"
#include "chips/Saa1099Decoder.h"
#include "chips/Sn76489Decoder.h"
#include "chips/WswanDecoder.h"
#include "chips/OplDecoder.h"
#include "chips/OpmDecoder.h"
#include "Formats/FormatsTests.h"
#include "Support/ChipTestSupport.h"
#include "Support/TestSupport.h"
#include "TestSettings.h"
#include "chips/OpnDecoder.h"

namespace
{
	using toybox::chips::NoteEvent;
	using toybox::chips::RegisterWrite;

	using toybox::test::check;

	using toybox::test::absoluteNote;
	using toybox::test::Stream;

	std::vector<NoteEvent> run (const Stream& stream, std::uint32_t totalSamples)
	{
		toybox::chips::NesApuDecoder decoder;
		decoder.reset (toybox::chips::ChipConfig { 1789773, 44100.0 });
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

		check (absoluteNote (notes[0]) == 69, "442 Hz decodes to A4");
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

		check (absoluteNote (notes[0]) == 69 && absoluteNote (notes[1]) == 57, "the second note is an octave lower");
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
		decoder.reset (toybox::chips::ChipConfig { 1789773, 44100.0 });
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
		check (absoluteNote (notes[0]) == 57, "the triangle plays A3");
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
		check (absoluteNote (notes[0]) == 40, "period index 4 maps to the snare key");
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
		decoder.reset (toybox::chips::ChipConfig { clock, 44100.0 });
		return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
	}

	/** Channel 1 at A4: fnum 0x21D, block 4 on a 7.67 MHz chip is 439.7 Hz. */
	void startFmNote (Stream& stream, std::uint32_t time, std::uint8_t algorithm,
					  std::uint8_t op1Level, std::uint8_t op4Level)
	{
		// In 0x40-0x4F the low two bits pick the channel and bits 2-3 the operator, so
		// channel 0's operators sit at 0x40 (op 1) and 0x4C (op 4); 0x43 is channel 3's
		// special-mode slot and the chip ignores it - writing operator 4 there left the
		// carrier at the silent default and the note vanished.
		stream.at (time, 0x40, op1Level);		// channel 0, operator 1 total level
		stream.at (time, 0x4C, op4Level);		// channel 0, operator 4 total level
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

		const auto notes = runChip<toybox::chips::OpnDecoder> (stream, 88200, kYmClock);

		check (notes.size() == 1, "one FM key-on is one note");
		if (notes.empty())
		{
			return;
		}

		check (absoluteNote (notes[0]) == 69, "fnum 0x21D at block 4 decodes to A4");
		check (notes[0].startSample == 0, "the note starts with the key-on");
		check (distance (notes[0].endSample, 44100) <= 64, "key off ends the note");
		check (notes[0].logicalChannel == 0, "the first FM channel is channel 0");
		check (notes[0].velocity == 127, "a carrier at total level 0 is full velocity");
	}

	void testFmSilentCarrier (void)
	{
		Stream stream;
		startFmNote (stream, 0, 0x00, 0x00, 0x7F);	// the only carrier is silenced

		const auto notes = runChip<toybox::chips::OpnDecoder> (stream, 44100, kYmClock);

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

		check (runChip<toybox::chips::OpnDecoder> (silent, 44100, kYmClock).empty(),
			   "algorithm 0 ignores the silent operator 4");
		check (runChip<toybox::chips::OpnDecoder> (audible, 44100, kYmClock).size() == 1,
			   "algorithm 7 outputs operator 1");
	}

	void testFmBlockIsAnOctave (void)
	{
		Stream stream;
		startFmNote (stream, 0, 0x00, 0x7F, 0x00);
		stream.at (22050, 0xA4, 0x2A);			// block 5, same f-number

		const auto notes = runChip<toybox::chips::OpnDecoder> (stream, 44100, kYmClock);

		check (notes.size() == 2, "a block change starts a second note");
		if (notes.size() == 2)
		{
			check (absoluteNote (notes[1]) == 81, "one block up is one octave up");
		}
	}

	/** Vibrato and slides rewrite the f-number without re-keying: the note stays, its pitch moves. */
	void testFmPitchBendFollowsTheFnumber (void)
	{
		Stream stream;
		startFmNote (stream, 0, 0x00, 0x7F, 0x00);
		stream.at (22050, 0xA0, 0x20);			// fnum 0x21D -> 0x220, about 10 cents up
		stream.at (44100, 0x28, 0x00);

		toybox::chips::OpnDecoder decoder;
		decoder.reset (toybox::chips::ChipConfig { kYmClock, 44100.0 });
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

		const auto notes = runChip<toybox::chips::OpnDecoder> (stream, 22050, kYmClock);

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

		const auto withoutDac = runChip<toybox::chips::OpnDecoder> (stream, 22050, kYmClock);

		stream.at (0, 0x2B, 0x80);					// DAC enable
		const auto withDac = runChip<toybox::chips::OpnDecoder> (stream, 22050, kYmClock);

		check (withoutDac.size() == 1 && withoutDac[0].logicalChannel == 5,
			   "channel 6 is an FM channel while the DAC is off");
		check (withDac.empty(), "the DAC replaces channel 6 instead of adding notes");
	}

	/** A three-channel OPN (YM2203) has no second port: register 0x28 with the port-select bit
		set addresses channel 3 of a six-channel OPN, which this chip does not have, while the
		low two bits still select one of its three. */
	void testOpnThreeChannels (void)
	{
		Stream stream;
		stream.at (0, 0x42, 0x00);					// channel 3 (index 2), operator 1
		stream.at (0, 0x4E, 0x00);					// channel 3 (index 2), operator 4
		stream.at (0, 0xA2, 0x1D);
		stream.at (0, 0xA6, 0x22);
		stream.at (0, 0xB2, 0x00);
		stream.at (0, 0x28, 0xF6);					// local channel 2, port bit set

		toybox::chips::OpnDecoder decoder (toybox::chips::OpnDecoder::Variant::ym2203);
		decoder.reset (toybox::chips::ChipConfig { kYmClock, 44100.0 });
		const auto notes = toybox::chips::extractNotes (decoder, stream.writes, 22050);

		check (notes.size() == 1 && notes[0].logicalChannel == 2,
			   "a YM2203 key-on stays inside its three channels");
		check (notes.size() == 1 && absoluteNote (notes[0]) == 69,
			   "the YM2203 shares the OPN f-number relation");
	}

	/** ADPCM-A and ADPCM-B are one-shots: the control bit starts them, the chip ends them. The
		decoder reports a pulse on the drum channel, not a held note. */
	void testOpnaAdpcmOneShots (void)
	{
		Stream stream;
		stream.at (0, 0, 0x10, 0x04);				// ADPCM-A channel 2 start (bit 2)
		stream.at (22050, 0, 0x10, 0x00);			// key off every ADPCM-A channel
		// DELTA-N first: it is ADPCM-B's pitch register, and a start with it at zero is silence
		stream.at (22050, 1, 0x09, 0x95);			// DELTA-N 8341, the scale a real driver uses
		stream.at (22050, 1, 0x0A, 0x20);
		stream.at (22050, 1, 0x0B, 0xFF);			// level
		stream.at (22050, 1, 0x00, 0x80);			// ADPCM-B start

		toybox::chips::OpnDecoder decoder (toybox::chips::OpnDecoder::Variant::ym2608);
		decoder.reset (toybox::chips::ChipConfig { 7987200, 44100.0 });
		const auto notes = toybox::chips::extractNotes (decoder, stream.writes, 44100);

		std::size_t adpcmA = 0;
		std::size_t adpcmB = 0;
		for (const auto& note : notes)
		{
			if (note.logicalChannel == 8)
			{
				++adpcmA;
			}
			if (note.logicalChannel == 12)
			{
				++adpcmB;
			}
		}

		check (adpcmA == 1, "an ADPCM-A start bit is one note on channels 6..11");
		check (adpcmB == 1, "an ADPCM-B start bit is one note on channel 12");
	}

	/** A tonal DELTA-T sample for the ADPCM-B tests: eight nibbles of +1 then eight of -1, which
		the decoder integrates into a triangle whose period is exactly 16 nibbles. */
	std::vector<std::uint8_t> makeAdpcmBTone (std::size_t size, std::size_t offset)
	{
		const std::uint8_t cycle[8] = { 0x00, 0x00, 0x00, 0x00, 0x88, 0x88, 0x88, 0x88 };

		std::vector<std::uint8_t> rom (size, 0);
		for (std::size_t i = 0; i + offset < size; ++i)
		{
			rom[offset + i] = cycle[i % 8];
		}
		return rom;
	}

	/** ADPCM-B is the one sampled voice on the OPN with a pitch register: DELTA-N. Its ROM holds
		4-bit deltas, so the sample's cycle only exists after decoding - and then the note follows
		DELTA-N, which is the "ADPCM-B root" the design doc asks for. */
	void testOpnAdpcmBPitch (void)
	{
		Stream stream;
		stream.at (0, 0, 0x12, 0x10);			// start 0x10 << 8 = 0x1000 (256-byte pages)
		stream.at (0, 0, 0x13, 0x00);
		stream.at (0, 0, 0x14, 0x11);			// end 0x11 << 8 | 0xFF = 0x11FF
		stream.at (0, 0, 0x15, 0x00);
		stream.at (0, 0, 0x19, 0x95);			// DELTA-N 8341: (clock/144)/65536 x 8341
		stream.at (0, 0, 0x1A, 0x20);			// = 7,073 nibbles a second, so A4 over 16 nibbles
		stream.at (0, 0, 0x1B, 0xFF);			// level, full scale
		stream.at (0, 0, 0x10, 0x80);			// start, no repeat

		toybox::chips::OpnDecoder decoder (toybox::chips::OpnDecoder::Variant::ym2610);
		decoder.reset (toybox::chips::ChipConfig { 8000000, 44100.0 });
		decoder.setAdpcmBRom (makeAdpcmBTone (0x4000, 0x1000));

		const auto notes = toybox::chips::extractNotes (decoder, stream.writes, 44100);

		check (notes.size() == 1, "an ADPCM-B start with a tonal sample is one note");
		if (notes.empty())
		{
			return;
		}

		check (notes[0].logicalChannel == 12, "ADPCM-B is channel 12");
		check (absoluteNote (notes[0]) == 69, "a 16-nibble cycle at 7080 nibbles a second is A4");
		check (notes[0].channelKind == toybox::chips::ChannelKind::opnAdpcmB,
			   "and its family says the pitch is real");
		check (! notes[0].isDrum, "so it is not a trigger");
	}

	/** DELTA-N is the pitch: the same sample at twice the register is an octave up. */
	void testOpnAdpcmBDeltaIsThePitch (void)
	{
		Stream stream;
		stream.at (0, 0, 0x12, 0x10);
		stream.at (0, 0, 0x13, 0x00);
		stream.at (0, 0, 0x14, 0x11);
		stream.at (0, 0, 0x15, 0x00);
		stream.at (0, 0, 0x19, 0x2A);			// twice the DELTA-N: 16682
		stream.at (0, 0, 0x1A, 0x41);
		stream.at (0, 0, 0x1B, 0xFF);
		stream.at (0, 0, 0x10, 0x80);

		toybox::chips::OpnDecoder decoder (toybox::chips::OpnDecoder::Variant::ym2610);
		decoder.reset (toybox::chips::ChipConfig { 8000000, 44100.0 });
		decoder.setAdpcmBRom (makeAdpcmBTone (0x4000, 0x1000));

		const auto notes = toybox::chips::extractNotes (decoder, stream.writes, 44100);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 81,
			   "twice DELTA-N is an octave up: A5 from the same sample");
	}

	/** The scale a real Neo Geo set uses: DELTA-N in the tens of thousands, not the two digits the
		first synthetic tests picked.

		This is the regression that kept every ADPCM-B track silent on real songs. The rate is
		`delta * (clock/144) / 65536`, so DELTA-N 38696 is 32,795 nibbles a second - but the note
		table once computed `delta * clock / 32768`, 288 times faster, which put the note past the top
		of the MIDI range. `noteFor` then returned -1 and the extractor dropped the channel, so the
		audio played and MIDI showed nothing. A note at all - let alone 84 - is the assertion that
		matters; without it the whole channel disappears. */
	void testOpnAdpcmBRealDeltaScale (void)
	{
		Stream stream;
		stream.at (0, 0, 0x12, 0x10);
		stream.at (0, 0, 0x13, 0x00);
		stream.at (0, 0, 0x14, 0x11);
		stream.at (0, 0, 0x15, 0x00);
		stream.at (0, 0, 0x19, 0x28);			// DELTA-N 38696, the figure Neo Geo drivers write
		stream.at (0, 0, 0x1A, 0x97);
		stream.at (0, 0, 0x1B, 0xFF);
		stream.at (0, 0, 0x10, 0x80);

		toybox::chips::OpnDecoder decoder (toybox::chips::OpnDecoder::Variant::ym2610);
		decoder.reset (toybox::chips::ChipConfig { 8000000, 44100.0 });
		decoder.setAdpcmBRom (makeAdpcmBTone (0x4000, 0x1000));

		const auto notes = toybox::chips::extractNotes (decoder, stream.writes, 44100);

		check (notes.size() == 1, "an ADPCM-B voice at a real DELTA-N still produces a note");
		if (notes.empty())
		{
			return;
		}

		check (! notes[0].isDrum, "and it is melodic - DELTA-N is a pitch register");
		check (notes[0].note == 84, "38696 is 32,795 nibbles a second: +2443 cents, so C6");
	}

	/** Level 0 is how the drivers end the voice (the Neo Geo stage theme writes it before every new
		start), so it closes the note rather than leaving it hanging. */
	void testOpnAdpcmBLevelEndsTheNote (void)
	{
		Stream stream;
		stream.at (0, 0, 0x12, 0x10);
		stream.at (0, 0, 0x13, 0x00);
		stream.at (0, 0, 0x14, 0x11);
		stream.at (0, 0, 0x15, 0x00);
		stream.at (0, 0, 0x19, 0x95);			// DELTA-N 8341, a real driver's scale
		stream.at (0, 0, 0x1A, 0x20);
		stream.at (0, 0, 0x1B, 0xFF);
		stream.at (0, 0, 0x10, 0x80);
		stream.at (20000, 0, 0x1B, 0x00);		// silence

		toybox::chips::OpnDecoder decoder (toybox::chips::OpnDecoder::Variant::ym2610);
		decoder.reset (toybox::chips::ChipConfig { 8000000, 44100.0 });
		decoder.setAdpcmBRom (makeAdpcmBTone (0x4000, 0x1000));

		const auto notes = toybox::chips::extractNotes (decoder, stream.writes, 44100);

		check (notes.size() == 1 && distance (notes[0].endSample, 20000) <= 64,
			   "level 0 ends the ADPCM-B note");
	}

	/** A sample with no period stays the C4 trigger: the drums share this voice. */
	void testOpnAdpcmBPercussion (void)
	{
		std::vector<std::uint8_t> rom (0x4000, 0);
		std::uint32_t seed = 24681357u;
		for (std::size_t i = 0; i < 512; ++i)
		{
			seed = seed * 1103515245u + 12345u;
			rom[0x1000 + i] = (std::uint8_t) (seed >> 16);
		}

		Stream stream;
		stream.at (0, 0, 0x12, 0x10);
		stream.at (0, 0, 0x13, 0x00);
		stream.at (0, 0, 0x14, 0x11);
		stream.at (0, 0, 0x15, 0x00);
		stream.at (0, 0, 0x19, 0x95);			// DELTA-N 8341, a real driver's scale
		stream.at (0, 0, 0x1A, 0x20);
		stream.at (0, 0, 0x1B, 0xFF);
		stream.at (0, 0, 0x10, 0x80);

		toybox::chips::OpnDecoder decoder (toybox::chips::OpnDecoder::Variant::ym2610);
		decoder.reset (toybox::chips::ChipConfig { 8000000, 44100.0 });
		decoder.setAdpcmBRom (rom);

		const auto notes = toybox::chips::extractNotes (decoder, stream.writes, 44100);

		// DELTA-N is a pitch register, so a set with no measurable period still has a note - it only
		// loses the root correction that would place the octave (the reference implementation maps
		// the rate alone, `searchYM2608Adpcm`). Treating it as a trigger was the old behaviour.
		check (notes.size() == 1 && ! notes[0].isDrum && notes[0].rootCents == 0,
			   "an ADPCM-B sample with no period keeps its DELTA-N pitch and loses only the root");
		check (notes.size() == 1 && notes[0].note == 58,
			   "and that pitch is the register's: DELTA-N 8341 is -208 cents, so A#3");
	}

	/** The OPNA addresses 32-byte pages where the OPNB uses 256: the same 0x1000 sample is start
		register 0x80 on a YM2608 and 0x10 on a YM2610. */
	void testOpnAdpcmBAddressShift (void)
	{
		Stream stream;
		stream.at (0, 1, 0x02, 0x80);			// start 0x80 << 5 = 0x1000
		stream.at (0, 1, 0x03, 0x00);
		stream.at (0, 1, 0x04, 0x91);			// end
		stream.at (0, 1, 0x05, 0x00);
		stream.at (0, 1, 0x09, 0xA2);			// DELTA-N 8354: the OPNA clock's own A4 scale
		stream.at (0, 1, 0x0A, 0x20);
		stream.at (0, 1, 0x0B, 0xFF);
		stream.at (0, 1, 0x00, 0x80);

		toybox::chips::OpnDecoder decoder (toybox::chips::OpnDecoder::Variant::ym2608);
		decoder.reset (toybox::chips::ChipConfig { 7987200, 44100.0 });
		decoder.setAdpcmBRom (makeAdpcmBTone (0x4000, 0x1000));

		const auto notes = toybox::chips::extractNotes (decoder, stream.writes, 44100);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 69,
			   "the OPNA reaches the same sample 32 bytes at a time: still A4 at its clock");
	}

	/** The non-B YM2610 has only channels 1, 2, 4 and 5 wired up; the B revision has all six.
		The same register stream must therefore sound one note on the first and two on the second. */
	void testOpnbFourChannelVariant (void)
	{
		Stream stream;
		for (int pair = 0; pair < 2; ++pair)
		{
			const std::uint8_t base = (std::uint8_t) (pair == 0 ? 0x40 : 0x41);
			const std::uint8_t op4 = (std::uint8_t) (pair == 0 ? 0x4C : 0x4D);
			const std::uint8_t low = (std::uint8_t) (pair == 0 ? 0xA0 : 0xA1);
			const std::uint8_t high = (std::uint8_t) (pair == 0 ? 0xA4 : 0xA5);
			const std::uint8_t algorithm = (std::uint8_t) (pair == 0 ? 0xB0 : 0xB1);
			stream.at (0, base, 0x00);
			stream.at (0, op4, 0x00);
			stream.at (0, low, 0x1D);
			stream.at (0, high, 0x22);
			stream.at (0, algorithm, 0x00);
		}
		stream.at (0, 0x28, 0xF0);					// channel 1
		stream.at (0, 0x28, 0xF1);					// channel 2

		toybox::chips::OpnDecoder fourChannel (toybox::chips::OpnDecoder::Variant::ym2610);
		fourChannel.reset (toybox::chips::ChipConfig { 8000000, 44100.0 });
		const auto four = toybox::chips::extractNotes (fourChannel, stream.writes, 22050);

		toybox::chips::OpnDecoder sixChannel (toybox::chips::OpnDecoder::Variant::ym2610b);
		sixChannel.reset (toybox::chips::ChipConfig { 8000000, 44100.0 });
		const auto six = toybox::chips::extractNotes (sixChannel, stream.writes, 22050);

		check (four.size() == 1 && four[0].logicalChannel == 1,
			   "the four-channel OPNB drops channel 0");
		check (six.size() == 2, "the B revision keys both channels");
	}

	/** OPM pitch is KC, not an f-number: block 4 with note code 10 is A4 at the reference
		clock, and a zero operator mask in 0x08 is the key-off. */
	void testOpmPitchAndKey (void)
	{
		Stream stream;
		stream.at (0, 0x28, 0x4A);			// block 4, note code 10
		stream.at (0, 0x08, 0x78);			// all four operators of channel 0
		stream.at (22050, 0x08, 0x00);		// key off

		toybox::chips::OpmDecoder decoder;
		decoder.reset (toybox::chips::ChipConfig { 3579545, 44100.0 });
		const auto notes = toybox::chips::extractNotes (decoder, stream.writes, 44100);

		check (notes.size() == 1, "one OPM key-on in, one note out");
		if (notes.empty())
		{
			return;
		}

		check (absoluteNote (notes[0]) == 69, "KC 0x4A decodes to A4 at the reference clock");
		check (notes[0].logicalChannel == 0, "OPM channel 0 is its own track");
		check (distance (notes[0].endSample, 22050) <= 64, "a zero operator mask ends the note");
	}

	/** The KC pitch is laid out for 3.579545 MHz; a chip running at twice that plays an octave
		higher for the same register value. */
	void testOpmClockScaling (void)
	{
		Stream stream;
		stream.at (0, 0x28, 0x4A);
		stream.at (0, 0x08, 0x78);

		toybox::chips::OpmDecoder decoder;
		decoder.reset (toybox::chips::ChipConfig { 7159090, 44100.0 });
		const auto notes = toybox::chips::extractNotes (decoder, stream.writes, 22050);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 81,
			   "a doubled OPM clock lifts the same KC by an octave");
	}

	/** OPL2: FNUM 0x244 with block 4 is A4 at 3.58 MHz, and bit 5 of B0 is the key. */
	void testOpl2PitchAndKey (void)
	{
		Stream stream;
		stream.at (0, 0xA0, 0x44);			// FNUM low (0x244)
		stream.at (0, 0xB0, 0x32);			// FNUM bit 9, block 4, key on
		stream.at (22050, 0xB0, 0x12);		// same pitch, key off

		toybox::chips::OplDecoder decoder (toybox::chips::OplDecoder::Variant::opl2);
		decoder.reset (toybox::chips::ChipConfig { 3579545, 44100.0 });
		const auto notes = toybox::chips::extractNotes (decoder, stream.writes, 44100);

		check (notes.size() == 1, "one OPL2 key-on in, one note out");
		if (notes.empty())
		{
			return;
		}

		check (absoluteNote (notes[0]) == 69, "OPL2 FNUM 0x244 block 4 decodes to A4");
		check (notes[0].logicalChannel == 0, "OPL2 channel 0 is its own track");
		check (distance (notes[0].endSample, 22050) <= 64, "clearing B0 bit 5 ends the note");
	}

	/** OPLL puts FNUM bit 8 and the block in 0x20..0x28 and its reference octave is one lower,
		so the same A4 needs FNUM 0x122 there. */
	void testOpllPitch (void)
	{
		Stream stream;
		stream.at (0, 0x10, 0x22);			// FNUM low (0x122)
		stream.at (0, 0x20, 0x19);			// FNUM bit 8, block 4, key on
		stream.at (11025, 0x30, 0x20);		// key off

		toybox::chips::OplDecoder decoder (toybox::chips::OplDecoder::Variant::opll);
		decoder.reset (toybox::chips::ChipConfig { 3579545, 44100.0 });
		const auto notes = toybox::chips::extractNotes (decoder, stream.writes, 44100);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 69,
			   "OPLL FNUM 0x122 block 4 decodes to A4");
		check (notes.size() == 1 && distance (notes[0].endSample, 11025) <= 64,
			   "0x30 bit 5 is the OPLL key-off");
	}

	/** An OPL3's second register port addresses the second nine-channel bank, which is the
		ninth track after the first one. */
	void testOpl3SecondBank (void)
	{
		Stream stream;
		stream.at (0, 1, 0xA0, 0x44);		// bank 1, channel 0
		stream.at (0, 1, 0xB0, 0x32);		// key on
		stream.at (11025, 1, 0xB0, 0x12);	// key off

		toybox::chips::OplDecoder decoder (toybox::chips::OplDecoder::Variant::opl3);
		decoder.reset (toybox::chips::ChipConfig { 14318180, 44100.0 });
		const auto notes = toybox::chips::extractNotes (decoder, stream.writes, 44100);

		check (notes.size() == 1 && notes[0].logicalChannel == 9,
			   "the OPL3 second port plays channel 9");
		check (notes.size() == 1 && absoluteNote (notes[0]) == 69,
			   "the OPL3 clock is compensated by its divisor");
	}

	/** Rhythm bits are level-triggered: setting bit 4 of 0xBD once is one bass-drum hit on the
		first rhythm track, which is channel 9 of an OPL2. */
	void testOplRhythm (void)
	{
		Stream stream;
		stream.at (0, 0xBD, 0x30);			// rhythm enable + bass drum
		stream.at (22050, 0xBD, 0x20);		// drum bit cleared

		toybox::chips::OplDecoder decoder (toybox::chips::OplDecoder::Variant::opl2);
		decoder.reset (toybox::chips::ChipConfig { 3579545, 44100.0 });
		const auto notes = toybox::chips::extractNotes (decoder, stream.writes, 44100);

		check (notes.size() == 1 && notes[0].logicalChannel == 9,
			   "an OPL2 rhythm bit is one hit on channel 9");
		check (notes.size() == 1 && notes[0].isDrum, "the rhythm track is a drum track");
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

		check (absoluteNote (notes[0]) == 69, "frequency 1750 decodes to A4");
		check (notes[0].startSample == 0, "the note starts with the trigger");
		check (distance (notes[0].endSample, 44100) <= 128, "the power switch ends the note");
		check (notes[0].velocity == 127, "envelope volume 15 is full velocity");
		check (notes[0].logicalChannel == 0, "pulse 1 is channel 0");
		check (notes[0].channelKind == toybox::chips::ChannelKind::gbPulse, "the note carries the GB family");
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

		check (absoluteNote (notes[0]) == 69, "the wave channel divides the clock by 64, so 1899 is A4");
		check (notes[0].logicalChannel == 2, "the wave channel is channel 2");
		check (notes[0].channelKind == toybox::chips::ChannelKind::gbWave, "the note carries the wave family");
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
		check (fastNotes[0].channelKind == toybox::chips::ChannelKind::gbNoise, "the note carries the noise family");
		check (absoluteNote (fastNotes[0]) == 38, "the fastest noise lands on the first drum key");
		check (slowNotes[0].note > fastNotes[0].note, "a slower noise lands on a higher drum key");
	}

	void testGbSilenceWithoutPower (void)
	{
		Stream stream;
		startGbA4 (stream, 0);				// never turns NR52 on

		check (runGb (stream, 22050).empty(), "an APU that was never powered on makes no notes");
	}

	// ---- HuC6280 (PC Engine) ----

	std::vector<NoteEvent> runC6280 (const Stream& stream, std::uint32_t totalSamples)
	{
		toybox::chips::HuC6280Decoder decoder;
		decoder.reset (toybox::chips::ChipConfig { 3579545, 44100.0 });
		return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
	}

	/** Register 0x00 selects the channel, 0x02/0x03 hold its 12-bit divider, 0x04 enables it. */
	void startC6280Note (Stream& stream, std::uint32_t time, std::uint8_t channel, int frequency)
	{
		stream.at (time, 0x00, channel);
		stream.at (time, 0x02, (std::uint8_t) (frequency & 0xFF));
		stream.at (time, 0x03, (std::uint8_t) ((frequency >> 8) & 0x0F));
		stream.at (time, 0x04, 0x9F);		// enable, volume 31
	}

	void testC6280WaveNote (void)
	{
		Stream stream;
		startC6280Note (stream, 0, 0, 254);		// 3579545 / (32 * 254) = 440.4 Hz

		const auto notes = runC6280 (stream, 22050);

		check (notes.size() == 1, "one wave note");
		if (notes.empty())
		{
			return;
		}

		check (absoluteNote (notes[0]) == 69, "divider 254 decodes to A4");
		check (notes[0].logicalChannel == 0, "the selected channel is channel 0");
		check (notes[0].channelKind == toybox::chips::ChannelKind::c6280Wave, "the note carries the wave family");
		check (notes[0].velocity == 127, "volume 31 is full velocity");
		check (! notes[0].isDrum, "a wave channel is melodic");
	}

	void testC6280SelectsOneChannel (void)
	{
		Stream stream;
		startC6280Note (stream, 0, 0, 254);		// A4
		startC6280Note (stream, 0, 2, 508);		// 3579545 / (32 * 508) = 220.2 Hz, A3

		const auto notes = runC6280 (stream, 22050);

		check (notes.size() == 2, "two selected channels play two notes");
		if (notes.size() != 2)
		{
			return;
		}

		check (absoluteNote (notes[0]) == 69 && notes[0].logicalChannel == 0,
			   "the first note keeps its own channel");
		check (absoluteNote (notes[1]) == 57 && notes[1].logicalChannel == 2,
			   "the high nibble of the divider reaches channel 2");
	}

	void testC6280NoiseIsADrum (void)
	{
		// channel 4 with noise on; the rate field counts down, so 0x1F is the fastest
		Stream fast;
		fast.at (0, 0x00, 0x04);
		fast.at (0, 0x04, 0x9F);
		fast.at (0, 0x07, 0x9F);

		Stream slow;
		slow.at (0, 0x00, 0x04);
		slow.at (0, 0x04, 0x9F);
		slow.at (0, 0x07, 0x80);

		const auto fastNotes = runC6280 (fast, 22050);
		const auto slowNotes = runC6280 (slow, 22050);

		check (fastNotes.size() == 1 && slowNotes.size() == 1, "one noise hit each");
		if (fastNotes.empty() || slowNotes.empty())
		{
			return;
		}

		check (fastNotes[0].isDrum, "the noise channel is a drum track");
		check (fastNotes[0].logicalChannel == 4, "the noise is channel 4");
		check (absoluteNote (fastNotes[0]) == 38, "the fastest noise lands on the first drum key");
		check (slowNotes[0].note > fastNotes[0].note, "a slower rate lands on a higher drum key");
	}

	void testC6280NoiseOnlyOnTheLastTwoChannels (void)
	{
		// the same register on channel 0: the core ignores it there, so the divider still decides
		Stream stream;
		startC6280Note (stream, 0, 0, 254);
		stream.at (0, 0x07, 0x9F);

		const auto notes = runC6280 (stream, 22050);

		check (notes.size() == 1, "channel 0 stays a tone channel");
		if (notes.empty())
		{
			return;
		}

		check (absoluteNote (notes[0]) == 69 && ! notes[0].isDrum, "the noise bit does nothing on channel 0");
	}

	// ---- K051649 / SCC ----

	std::vector<NoteEvent> runScc (const Stream& stream, std::uint32_t totalSamples)
	{
		toybox::chips::K051649Decoder decoder;
		decoder.reset (toybox::chips::ChipConfig { 3579545, 44100.0 });
		return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
	}

	/** Every SCC access is a latch (even offset) and then a value (the offset after it). */
	void setSccRegister (Stream& stream, std::uint32_t time, std::uint8_t group, std::uint8_t latch,
						 std::uint8_t value)
	{
		stream.at (time, (std::uint8_t) (group * 2), latch);
		stream.at (time, (std::uint8_t) (group * 2 + 1), value);
	}

	void testSccWaveNote (void)
	{
		Stream stream;
		setSccRegister (stream, 0, 0x02, 0x00, 0x0F);		// volume 15 on channel 0
		setSccRegister (stream, 0, 0x01, 0x00, 0xFC);		// divider low: 0xFC = 252
		setSccRegister (stream, 0, 0x01, 0x01, 0x01);		// divider high: 0x1FC = 508
		setSccRegister (stream, 0, 0x03, 0x00, 0x01);		// key on channel 0

		const auto notes = runScc (stream, 22050);

		check (notes.size() == 1, "the key mask starts one note");
		if (notes.empty())
		{
			return;
		}

		// 3579545 / (16 * 509) = 439.5 Hz
		check (absoluteNote (notes[0]) == 69, "divider 508 is A4");
		check (notes[0].logicalChannel == 0, "the latch selected channel 0");
		check (notes[0].channelKind == toybox::chips::ChannelKind::sccWave, "the note carries the wave family");
		check (notes[0].velocity == 127, "volume 15 is full velocity");
	}

	void testSccKeyMaskEndsTheNote (void)
	{
		Stream stream;
		setSccRegister (stream, 0, 0x02, 0x00, 0x0F);
		setSccRegister (stream, 0, 0x01, 0x00, 0xFC);
		setSccRegister (stream, 0, 0x01, 0x01, 0x01);
		setSccRegister (stream, 0, 0x03, 0x00, 0x01);
		setSccRegister (stream, 22050, 0x03, 0x00, 0x00);	// key off

		const auto notes = runScc (stream, 44100);

		check (notes.size() == 1, "the key mask, not a timer, ends the note");
		if (notes.empty())
		{
			return;
		}

		check (distance (notes[0].endSample, 22050) <= 128, "the note ends with the key off");
	}

	void testSccLatchPicksTheChannel (void)
	{
		Stream stream;
		setSccRegister (stream, 0, 0x02, 0x01, 0x0F);		// volume 15 on channel 1
		setSccRegister (stream, 0, 0x01, 0x02, 0xFE);		// channel 1, low byte 254
		setSccRegister (stream, 0, 0x01, 0x03, 0x00);		// channel 1, high nibble 0
		setSccRegister (stream, 0, 0x03, 0x00, 0x02);		// key on channel 1

		const auto notes = runScc (stream, 22050);

		check (notes.size() == 1, "one note from the other channel");
		if (notes.empty())
		{
			return;
		}

		// 3579545 / (16 * 255) = 877.3 Hz
		check (absoluteNote (notes[0]) == 81, "divider 254 is A5");
		check (notes[0].logicalChannel == 1, "the frequency latch selected channel 1");
	}

	void testSccSilentWithoutVolume (void)
	{
		Stream stream;
		setSccRegister (stream, 0, 0x01, 0x00, 0xFC);
		setSccRegister (stream, 0, 0x01, 0x01, 0x01);
		setSccRegister (stream, 0, 0x03, 0x00, 0x01);		// key on, volume still 0

		check (runScc (stream, 22050).empty(), "a keyed channel at volume 0 makes no note");
	}

	// ---- SAA1099 ----

	std::vector<NoteEvent> runSaa (const Stream& stream, std::uint32_t totalSamples)
	{
		toybox::chips::Saa1099Decoder decoder;
		decoder.reset (toybox::chips::ChipConfig { 8000000, 44100.0 });
		return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
	}

	void testSaaSquareNote (void)
	{
		Stream stream;
		stream.at (0, 0x00, 0xFF);			// amplitude: both sides 15
		stream.at (0, 0x08, 227);			// frequency 227 at octave 3
		stream.at (0, 0x10, 0x33);			// octave 3 for both channels of the pair
		stream.at (0, 0x14, 0x01);			// tone on channel 0
		stream.at (0, 0x1C, 0x01);			// the chip-wide enable

		const auto notes = runSaa (stream, 22050);

		check (notes.size() == 1, "one square note");
		if (notes.empty())
		{
			return;
		}

		// 8000000 * 8 / (512 * (511 - 227)) = 440.1 Hz
		check (absoluteNote (notes[0]) == 69, "octave 3 and frequency 227 are A4");
		check (notes[0].logicalChannel == 0, "the note is on channel 0");
		check (notes[0].channelKind == toybox::chips::ChannelKind::saaSquare, "the note carries the SAA family");
		check (notes[0].velocity == 127, "amplitude 15 is full velocity");
		check (! notes[0].isDrum, "a tone channel is melodic");
	}

	void testSaaOctaveIsAnOctave (void)
	{
		Stream low;
		low.at (0, 0x00, 0xFF);
		low.at (0, 0x08, 227);
		low.at (0, 0x10, 0x33);
		low.at (0, 0x14, 0x01);
		low.at (0, 0x1C, 0x01);

		Stream high = low;
		high.writes[2].value = 0x44;		// octave 4

		const auto lowNotes = runSaa (low, 22050);
		const auto highNotes = runSaa (high, 22050);

		check (lowNotes.size() == 1 && highNotes.size() == 1, "one note each");
		if (lowNotes.empty() || highNotes.empty())
		{
			return;
		}

		check (highNotes[0].note == lowNotes[0].note + 12, "one octave up doubles the pitch");
	}

	void testSaaNeedsTheChipEnable (void)
	{
		Stream stream;
		stream.at (0, 0x00, 0xFF);
		stream.at (0, 0x08, 227);
		stream.at (0, 0x10, 0x33);
		stream.at (0, 0x14, 0x01);			// everything but register 0x1C

		check (runSaa (stream, 22050).empty(), "a disabled chip is silent");
	}

	void testSaaNoiseOnlyChannelIsADrum (void)
	{
		Stream stream;
		stream.at (0, 0x00, 0xFF);
		stream.at (0, 0x15, 0x01);			// noise on channel 0
		stream.at (0, 0x16, 0x00);			// both noise generators on their fastest rate
		stream.at (0, 0x1C, 0x01);

		const auto notes = runSaa (stream, 22050);

		check (notes.size() == 1, "noise alone still carries a note");
		if (notes.empty())
		{
			return;
		}

		check (notes[0].isDrum, "a noise-only channel is a drum track");
		check (absoluteNote (notes[0]) == 38, "the fastest noise generator lands on the first drum key");
	}

	void testSaaNoiseRatePicksTheDrumKey (void)
	{
		Stream slow;
		slow.at (0, 0x00, 0xFF);
		slow.at (0, 0x15, 0x01);
		slow.at (0, 0x16, 0x02);			// generator 0 at its slowest rate
		slow.at (0, 0x1C, 0x01);

		const auto notes = runSaa (slow, 22050);

		check (notes.size() == 1, "one drum hit");
		if (notes.empty())
		{
			return;
		}

		check (absoluteNote (notes[0]) == 42, "a slower generator lands on a higher drum key");
	}

	// ---- POKEY ----

	std::vector<NoteEvent> runPokey (const Stream& stream, std::uint32_t totalSamples)
	{
		toybox::chips::PokeyDecoder decoder;
		decoder.reset (toybox::chips::ChipConfig { 1789772, 44100.0 });
		return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
	}

	void testPokeySquareNote (void)
	{
		Stream stream;
		stream.at (0, 0x00, 72);			// AUDF1: divider 73
		stream.at (0, 0x01, 0x0F);			// AUDC1: distortion 0, volume 15

		const auto notes = runPokey (stream, 22050);

		check (notes.size() == 1, "one divider note");
		if (notes.empty())
		{
			return;
		}

		// (1789772 / 28) / (2 * 73) = 437.8 Hz
		check (absoluteNote (notes[0]) == 69, "divider 72 in the 64 kHz mode is A4");
		check (notes[0].logicalChannel == 0, "the note is on channel 0");
		check (notes[0].channelKind == toybox::chips::ChannelKind::pokeySquare, "the note carries the POKEY family");
		check (notes[0].velocity == 127, "volume 15 is full velocity");
	}

	void testPokeyClockModeChangesThePitch (void)
	{
		Stream fast;
		fast.at (0, 0x00, 72);
		fast.at (0, 0x01, 0x0F);

		Stream slow = fast;
		slow.at (0, 0x08, 0x01);			// AUDCTL: 15 kHz base clock

		const auto fastNotes = runPokey (fast, 22050);
		const auto slowNotes = runPokey (slow, 22050);

		check (fastNotes.size() == 1 && slowNotes.size() == 1, "one note each");
		if (fastNotes.empty() || slowNotes.empty())
		{
			return;
		}

		// 114 / 28 is two octaves and a bit: the same divider lands far lower
		check (slowNotes[0].note < fastNotes[0].note - 12, "the 15 kHz clock lowers the pitch");
	}

	void testPokeyJoinedPairIsOneVoice (void)
	{
		Stream stream;
		stream.at (0, 0x00, 0x00);			// AUDF1 low byte 0
		stream.at (0, 0x01, 0x0F);			// AUDC1 volume 15
		stream.at (0, 0x02, 0x01);			// AUDF2 high byte 1
		stream.at (0, 0x08, 0x10);			// AUDCTL: join channels 1 and 2

		const auto notes = runPokey (stream, 22050);

		check (notes.size() == 1, "a joined pair is one voice");
		if (notes.empty())
		{
			return;
		}

		// 16-bit divider 0x100 + 1; 63920 / (2 * 257) = 124.4 Hz
		check (notes[0].logicalChannel == 0, "the pair reports on its low channel");
		check (absoluteNote (notes[0]) == 47, "the 16-bit divider is B2");
	}

	void testPokeyVolumeOnlyIsSilent (void)
	{
		Stream stream;
		stream.at (0, 0x00, 72);
		stream.at (0, 0x01, 0x1F);			// AUDC1 bit 4: a DC level, not a tone

		check (runPokey (stream, 22050).empty(), "volume-only mode makes no note");
		}

		// ---- WonderSwan ----

		std::vector<NoteEvent> runWswan (const Stream& stream, std::uint32_t totalSamples,
										 std::uint32_t clock = 3072000)
		{
			toybox::chips::WswanDecoder decoder;
			decoder.reset (toybox::chips::ChipConfig { clock, 44100.0 });
			return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
		}

		void testWswanWaveNote (void)
		{
			Stream stream;
			stream.at (0, 0x80, 0x26);			// channel 0 divider 1830
			stream.at (0, 0x81, 0x07);
			stream.at (0, 0x88, 0xFF);			// full volume on both sides
			stream.at (0, 0x90, 0x01);			// channel 0 on

			const auto notes = runWswan (stream, 22050);

			check (notes.size() == 1, "one WonderSwan wave note");
			if (notes.empty())
			{
				return;
			}

			// 3072000 / (32 * (2048 - 1830)) = 440.4 Hz
			check (absoluteNote (notes[0]) == 69, "divider 1830 is A4");
			check (notes[0].logicalChannel == 0, "the note is on channel 0");
			check (notes[0].channelKind == toybox::chips::ChannelKind::wsWave,
				   "the note carries the WonderSwan family");
			check (notes[0].velocity == 127, "volume 15 is full velocity");

			const auto fallback = runWswan (stream, 22050, 0);
			check (fallback.size() == 1 && absoluteNote (fallback[0]) == 69,
				   "a header without a clock falls back to 3.072 MHz");
		}

		void testWswanEnableBitEndsTheNote (void)
		{
			Stream stream;
			stream.at (0, 0x80, 0x26);
			stream.at (0, 0x81, 0x07);
			stream.at (0, 0x88, 0xFF);
			stream.at (0, 0x90, 0x01);
			stream.at (11025, 0x90, 0x00);		// clear the enable bit: no key-off register exists

			const auto notes = runWswan (stream, 22050);

			check (notes.size() == 1, "clearing the enable bit ends the note");
			if (notes.empty())
			{
				return;
			}

			// the extractor polls every 64 samples, so the end lands on the next poll
			check (notes[0].endSample >= 11025 && notes[0].endSample <= 11089,
				   "the note ends where the bit was cleared");
		}

		void testWswanSilentWithoutVolume (void)
		{
			Stream stream;
			stream.at (0, 0x80, 0x26);
			stream.at (0, 0x81, 0x07);
			stream.at (0, 0x88, 0x00);			// volume 0
			stream.at (0, 0x90, 0x01);

			check (runWswan (stream, 22050).empty(), "volume 0 makes no note");
		}

		void testWswanNoiseIsADrum (void)
		{
			Stream stream;
			stream.at (0, 0x86, 0x00);			// channel 3 divider 0: the slowest noise rate
			stream.at (0, 0x87, 0x00);
			stream.at (0, 0x8B, 0xFF);
			stream.at (0, 0x90, 0x88);			// channel 3 on, noise on

			const auto notes = runWswan (stream, 22050);

			check (notes.size() == 1, "the noise generator makes one drum note");
			if (notes.empty())
			{
				return;
			}

			check (absoluteNote (notes[0]) == 38, "the slowest noise rate is the base drum key");
			check (notes[0].isDrum, "the noise channel is a drum");
			check (notes[0].channelKind == toybox::chips::ChannelKind::wsNoise,
				   "the note carries the noise family");

			Stream stopped;
			stopped.at (0, 0x86, 0xFF);			// 0xFFFF: the core stops the LFSR too
			stopped.at (0, 0x87, 0xFF);
			stopped.at (0, 0x8B, 0xFF);
			stopped.at (0, 0x90, 0x88);

			check (runWswan (stopped, 22050).empty(), "a stopped noise divider is silent");
		}

		void testWswanNoiseRatePicksTheDrumKey (void)
		{
			Stream slow;
			slow.at (0, 0x86, 0x00);
			slow.at (0, 0x87, 0x00);
			slow.at (0, 0x8B, 0xFF);
			slow.at (0, 0x90, 0x88);

			Stream fast = slow;
			fast.at (0, 0x86, 0xFF);			// channel 3 divider 2047: the fastest rate
			fast.at (0, 0x87, 0x07);

			const auto slowNotes = runWswan (slow, 22050);
			const auto fastNotes = runWswan (fast, 22050);

			check (slowNotes.size() == 1 && fastNotes.size() == 1, "one drum note each");
			if (slowNotes.empty() || fastNotes.empty())
			{
				return;
			}

			check (fastNotes[0].note > slowNotes[0].note + 12, "a faster generator lands on a higher key");
		}

		void testWswanSweepMovesThePitch (void)
		{
			Stream stream;
			stream.at (0, 0x84, 0x26);			// channel 2 divider 1830 = A4
			stream.at (0, 0x85, 0x07);
			stream.at (0, 0x8A, 0xFF);
			stream.at (0, 0x8C, 0xFF);			// sweep step -1
			stream.at (0, 0x8D, 0x00);			// one step every 8192 clocks (~118 samples)
			stream.at (0, 0x90, 0x44);			// channel 2 on, sweep on

			const auto notes = runWswan (stream, 22050);

			check (notes.size() > 2, "a running sweep walks the pitch down through several notes");
			if (notes.size() < 2)
			{
				return;
			}

			check (notes.back().note < notes.front().note - 6, "a negative step lowers the pitch");
		}

		void testWswanVoiceIsAOneShot (void)
		{
			Stream stream;
			stream.at (0, 0x94, 0xFF);			// PCM volume 12/12
			stream.at (0, 0x90, 0x22);			// channel 1 on, voice mode
			stream.at (441, 0x89, 0x80);		// a sound DMA byte

			const auto notes = runWswan (stream, 22050);

			check (notes.size() == 1, "the DMA stream is one trigger, not one note per byte");
			if (notes.empty())
			{
				return;
			}

			check (absoluteNote (notes[0]) == 60, "the voice is a C4 marker");
			check (notes[0].endSample <= 4480, "the marker lasts about 100 ms");
			check (notes[0].channelKind == toybox::chips::ChannelKind::adpcmDrum, "the voice is a sample");
		}

		// ---- SN76489 ----

		std::vector<NoteEvent> runSn (const Stream& stream, std::uint32_t totalSamples,
									  std::uint32_t clock = 3579545)
		{
			toybox::chips::Sn76489Decoder decoder;
			decoder.reset (toybox::chips::ChipConfig { clock, 44100.0 });
			return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
		}

		/** Channel 0, divider 254: 3579545 / (32 * 254) = 440 Hz. */
		void testSnToneNote (void)
		{
			Stream stream;
			stream.at (0, 0x8E, 0x00);			// latch: channel 0, tone, low nibble 0xE
			stream.at (0, 0x0F, 0x00);			// high six bits: divider 0x0FE
			stream.at (0, 0x90, 0x00);			// channel 0, volume: attenuation 0
			stream.at (22050, 0x9F, 0x00);		// attenuation 15 ends the note

			const auto notes = runSn (stream, 44100);

			check (notes.size() == 1, "one SN76489 tone in, one note out");
			if (notes.empty())
			{
				return;
			}

			check (absoluteNote (notes[0]) == 69, "SN76489 divider 254 decodes to A4");
			check (notes[0].logicalChannel == 0, "SN76489 channel 0 is its own track");
			check (distance (notes[0].endSample, 22050) <= 64, "attenuation 15 ends the note");
		}

		/** The divider is written in two bytes; a lone latch byte must not be read as a whole
			divider, or the second write would shorten the note by an octave instead. */
		void testSnDividerNeedsBothBytes (void)
		{
			Stream stream;
			stream.at (0, 0xAE, 0x00);			// channel 1, tone, low nibble 0xE
			stream.at (0, 0x0F, 0x00);			// high six bits: divider 0x0FE
			stream.at (0, 0xB0, 0x00);			// channel 1 volume

			const auto notes = runSn (stream, 22050);

			check (notes.size() == 1 && absoluteNote (notes[0]) == 69,
				   "the high divider byte belongs to the latch channel");
			check (notes.size() == 1 && notes[0].logicalChannel == 1,
				   "SN76489 channel 1 is its own track");
		}

		/** The noise generator has no pitch: its mode picks a GM drum key. */
		void testSnNoiseIsADrum (void)
		{
			Stream stream;
			stream.at (0, 0xE4, 0x00);			// channel 3: noise, white, rate 0
			stream.at (0, 0xF0, 0x00);			// channel 3 volume

			const auto notes = runSn (stream, 22050);

			check (notes.size() == 1, "one SN76489 noise mode in, one drum note out");
			if (notes.empty())
			{
				return;
			}

			check (notes[0].note >= 35 && notes[0].note <= 81, "the noise mode lands on a drum key");
			check (notes[0].isDrum, "SN76489 channel 3 is a drum track");
			check (notes[0].channelKind == toybox::chips::ChannelKind::sn76489Noise, "the noise has its own family");
		}

		/** Sega parts stop at divider 0 instead of folding it to 0x400, so it must be silence. */
		void testSnZeroDividerIsSilent (void)
		{
			Stream stream;
			stream.at (0, 0x80, 0x00);			// divider low nibble 0
			stream.at (0, 0x00, 0x00);			// divider high bits 0
			stream.at (0, 0x90, 0x00);			// volume

			check (runSn (stream, 22050).empty(), "SN76489 divider 0 is silent");
		}

		// ---- AY-3-8910 ----

		std::vector<NoteEvent> runAy (const Stream& stream, std::uint32_t totalSamples,
									  std::uint32_t clock = 1789773)
		{
			toybox::chips::Ay8910Decoder decoder;
			decoder.reset (toybox::chips::ChipConfig { clock, 44100.0 });
			return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
		}

		/** Channel A, divider 254: 1789773 / (16 * 254) = 440 Hz. */
		void testAyToneNote (void)
		{
			Stream stream;
			stream.at (0, 0x00, 0xFE);			// tone period low
			stream.at (0, 0x01, 0x00);			// tone period high, 12 bits
			stream.at (0, 0x07, 0x3E);			// tone A on, everything else off
			stream.at (0, 0x08, 0x0F);			// channel A amplitude 15

			const auto notes = runAy (stream, 22050);

			check (notes.size() == 1, "one AY tone in, one note out");
			if (notes.empty())
			{
				return;
			}

			check (absoluteNote (notes[0]) == 69, "AY divider 254 decodes to A4");
			check (notes[0].logicalChannel == 0, "AY channel A is its own track");
			check (notes[0].channelKind == toybox::chips::ChannelKind::aySquare, "the AY square has its own family");
		}

		/** A set mixer bit disables the tone: the register is an enable mask, not a volume. */
		void testAyMixerBitEndsTheNote (void)
		{
			Stream stream;
			stream.at (0, 0x00, 0xFE);
			stream.at (0, 0x01, 0x00);
			stream.at (0, 0x07, 0x3E);
			stream.at (0, 0x08, 0x0F);
			stream.at (11025, 0x07, 0x3F);		// disable tone A

			const auto notes = runAy (stream, 22050);

			check (notes.size() == 1 && distance (notes[0].endSample, 11025) <= 64,
				   "setting the mixer bit ends the note");
		}

		void testAySilentAmplitude (void)
		{
			Stream stream;
			stream.at (0, 0x00, 0xFE);
			stream.at (0, 0x01, 0x00);
			stream.at (0, 0x07, 0x3E);
			stream.at (0, 0x08, 0x00);			// amplitude 0

			check (runAy (stream, 22050).empty(), "AY amplitude 0 is silent");
		}

		// ---- RF5C68 ----

		std::vector<NoteEvent> runRf5c (const Stream& stream, std::uint32_t totalSamples)
		{
			toybox::chips::Rf5c68Decoder decoder;
			decoder.reset (toybox::chips::ChipConfig { 12500000, 44100.0 });
			return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
		}

		/** Bit 0 of the channel mask cleared enables channel 0; the sample has no pitch, so the
			note is a C4 marker closed by the decoder's own gate. */
		void testRf5cChannelIsAOneShot (void)
		{
			Stream stream;
			stream.at (0, 0x00, 0xFF);			// envelope: a zero here is silence, as on the chip
			stream.at (0, 0x08, 0xFE);			// channel 0 enabled

			const auto notes = runRf5c (stream, 44100);

			check (notes.size() == 1, "one RF5C68 channel enable is one sample note");
			if (notes.empty())
			{
				return;
			}

			check (absoluteNote (notes[0]) == 60, "the sample is a C4 marker");
			check (notes[0].logicalChannel == 0, "RF5C68 channel 0 is its own track");
			check (notes[0].isDrum, "the sample is a drum track");
			check (distance (notes[0].endSample, 3528) <= 64, "a driver that never disables still ends");
		}

		void testRf5cMaskSelectsTheChannel (void)
		{
			Stream stream;
			stream.at (0, 0x07, 0xC1);			// bit 7 output on, bit 6: the low registers are channel 1
			stream.at (0, 0x00, 0xFF);			// envelope
			stream.at (0, 0x08, 0xFD);			// channel 1 only

			const auto notes = runRf5c (stream, 22050);

			check (notes.size() == 1 && notes[0].logicalChannel == 1,
				   "only the unmasked channel plays");
		}

		void testRf5cMasterSwitchEndsTheNote (void)
		{
			Stream stream;
			stream.at (0, 0x00, 0xFF);			// envelope
			stream.at (0, 0x08, 0xFE);			// channel 0 enabled
			stream.at (1323, 0x07, 0x00);		// sound output off before the gate expires

			const auto notes = runRf5c (stream, 22050);

			check (notes.size() == 1 && distance (notes[0].endSample, 1323) <= 64,
				   "the master switch ends a sounding channel");
		}

		// --- X1-010: one channel, two instruments ---------------------------------------------

		std::vector<NoteEvent> runX1 (const Stream& stream, std::uint32_t totalSamples)
		{
			toybox::chips::X1_010Decoder decoder;
			decoder.reset (toybox::chips::ChipConfig { 16000000, 44100.0 });
			return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
		}

		/** Waveform mode: pitch is a 6.10 value over a 128-byte table, so one cycle is
			clock * pitch / 2^26 Hz. 0x0735 = 1845 at 16 MHz is 439.9 Hz - A4. */
		void testX1WaveNote (void)
		{
			Stream stream;
			stream.at (0, 0x02, 0x35);			// pitch lo
			stream.at (0, 0x03, 0x07);			// pitch hi
			stream.at (10, 0x00, 0x03);			// waveform mode + key on
			stream.at (8000, 0x00, 0x02);		// key off

			const auto notes = runX1 (stream, 12000);

			check (notes.size() == 1, "an X1-010 waveform key-on is one note");
			if (notes.empty())
			{
				return;
			}

			check (absoluteNote (notes[0]) == 69, "0x0735 of 6.10 pitch at 16 MHz is A4");
			check (notes[0].logicalChannel == 0, "X1-010 channel 0 is its own track");
			check (! notes[0].isDrum, "a wavetable voice is a melodic track");
			check (distance (notes[0].endSample, 8000) <= 2, "the key-off ends the note");
		}

		/** Status bit 7 halves the pitch (the "divider flag" the hardware notes call out). */
		void testX1DividerHalvesTheFrequency (void)
		{
			Stream stream;
			stream.at (0, 0x02, 0x35);
			stream.at (0, 0x03, 0x07);
			stream.at (10, 0x00, 0x83);			// divider + waveform + key on
			stream.at (8000, 0x00, 0x82);		// key off

			const auto notes = runX1 (stream, 12000);

			check (notes.size() == 1 && absoluteNote (notes[0]) == 57, "the divider flag drops the note an octave");
		}

		/** PCM mode: the same channel plays a sample, and the note is a C4 marker the decoder's
			own 80 ms gate closes - a driver that never keys off still gets one hit. */
		void testX1PcmIsAOneShot (void)
		{
			Stream stream;
			stream.at (0, 0x01, 0x77);			// PCM volume, both sides
			stream.at (10, 0x00, 0x01);			// PCM mode + key on
			stream.at (9000, 0x06, 0x00);		// a reserved write, only to move the clock on

			const auto notes = runX1 (stream, 12000);

			check (notes.size() == 1, "one PCM key-on is one sample note");
			if (notes.empty())
			{
				return;
			}

			check (absoluteNote (notes[0]) == 60, "the sample is a C4 marker");
			check (notes[0].isDrum, "a PCM channel is a drum track");
			check (distance (notes[0].endSample, 3528) <= 64, "the 80 ms gate ends the sample");
		}

		/** Offsets from 0x80 up are envelope and wavetable data: uploading a waveform must not
			look like a key-on on channel 0. */
		void testX1EnvelopeWritesDoNotKey (void)
		{
			Stream stream;
			stream.at (0, 0x01, 0x00);			// high byte of the offset
			stream.at (0, 0x00, 0x03);			// offset 0x0100: wavetable data
			stream.at (10, 0x01, 0x00, 0x0F);

			const auto notes = runX1 (stream, 2000);

			check (notes.empty(), "waveform data does not key a channel on");
		}

		// --- YMZ280B: a latch, then a key-on ------------------------------------------------

		std::vector<NoteEvent> runYmz (const Stream& stream, std::uint32_t totalSamples)
		{
			toybox::chips::Ymz280bDecoder decoder;
			decoder.reset (toybox::chips::ChipConfig { 16934400, 44100.0 });
			return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
		}

		/** VGM's 0x5D carries the register number in `aa` and the value in `dd` - the shape a real
			rip uses (0x5D 0x0D 0x20, 0x5D 0x6C 0xBD, ...), not a latch pair. */
		void writeYmz (Stream& stream, std::uint32_t time, std::uint8_t reg, std::uint8_t value)
		{
			stream.at (time, reg, value);
		}

		/** Voice 0: level, then key on. The sample has no note of its own, so it is a C4 marker
			closed by the decoder's own gate. */
		void testYmzChannelIsAOneShot (void)
		{
			Stream stream;
			writeYmz (stream, 0, 0x02, 0xFF);		// total level, full
			writeYmz (stream, 10, 0x01, 0x80);		// key on

			const auto notes = runYmz (stream, 12000);

			check (notes.size() == 1, "one YMZ280B key-on is one sample note");
			if (notes.empty())
			{
				return;
			}

			check (absoluteNote (notes[0]) == 60, "the sample is a C4 marker");
			check (notes[0].logicalChannel == 0, "voice 0 is its own track");
			check (notes[0].isDrum, "a PCM voice is a drum track");
			check (distance (notes[0].endSample, 3528 + 10) <= 64, "the 80 ms gate ends the sample");
		}

		/** Register 0x05 is the next voice up: (reg >> 2) & 7 picks the voice, so 0x04/0x05 are
			voice 1. A latch that leaked across voices would put this note on voice 0. */
		void testYmzLatchSelectsTheVoice (void)
		{
			Stream stream;
			writeYmz (stream, 0, 0x06, 0xFF);		// voice 1 level
			writeYmz (stream, 10, 0x05, 0x80);		// voice 1 key on

			const auto notes = runYmz (stream, 4000);

			check (notes.size() == 1 && notes[0].logicalChannel == 1, "register 0x05 keys voice 1");
		}

		/** A voice whose level is zero makes no sound, so it must not make a note either. */
		void testYmzSilentLevelIsNoNote (void)
		{
			Stream stream;
			writeYmz (stream, 10, 0x01, 0x80);		// key on with the level left at 0

			const auto notes = runYmz (stream, 8000);

			check (notes.empty(), "a voice at level 0 does not key a note");
		}

		// --- YMF271 / OPX: twelve groups, four sync modes ----------------------------------

		std::vector<NoteEvent> runOpx (const Stream& stream, std::uint32_t totalSamples)
		{
			toybox::chips::Ymf271Decoder decoder;
			decoder.reset (toybox::chips::ChipConfig { 16934400, 44100.0 });
			return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
		}

		/** One FM register write: the address byte is register << 4 | group. */
		void writeOpx (Stream& stream, std::uint32_t time, std::uint8_t port, std::uint8_t reg,
					   std::uint8_t group, std::uint8_t value)
		{
			stream.at (time, port, (std::uint8_t) ((reg << 4) | group), value);
		}

		/** A four-operator group at block 4, fns 0x51C, multiple 1: 440 Hz, A4. The pitch and the
			algorithm go to bank 0, where the group copies them to the other operators. */
		void startOpxFm (Stream& stream, std::uint8_t group, std::uint8_t algorithm)
		{
			for (std::uint8_t port = 0; port < 4; ++port)
			{
				writeOpx (stream, 0, port, 0x3, group, 0x01);		// multiple 1 on every operator
			}
			writeOpx (stream, 0, 0, 0xA, group, 0x45);				// block 4, fns high nibble 5
			writeOpx (stream, 0, 0, 0x9, group, 0x1C);				// fns low byte
			writeOpx (stream, 0, 0, 0xC, group, algorithm);			// copies to the group
		}

		void testOpxFmNote (void)
		{
			Stream stream;
			startOpxFm (stream, 0, 0x08);						// algorithm 8: S1 and S4 carry
			writeOpx (stream, 100, 0, 0x0, 0, 0x01);			// key on
			writeOpx (stream, 8000, 0, 0x0, 0, 0x00);			// key off

			const auto notes = runOpx (stream, 12000);

			check (notes.size() == 1, "one OPX key-on is one note");
			if (notes.empty())
			{
				return;
			}

			check (absoluteNote (notes[0]) == 69, "block 4, fns 0x51C, multiple 1 is A4");
			check (notes[0].logicalChannel == 0, "group 0 is its own track");
			check (notes[0].channelKind == toybox::chips::ChannelKind::fm, "the note carries the FM family");
			check (! notes[0].isDrum, "a four-operator group is melodic");
			check (notes[0].velocity == 127, "total level 0 is full velocity");
			check (distance (notes[0].endSample, 8000) <= 64, "the key-off ends the note");
		}

		/** The algorithm, not the bank order, decides what reaches the output: under algorithm 5
			only S4 (bank 3) carries, so silencing it leaves the group silent; algorithm 8 brings
			S1 (bank 0) in and the same silenced S4 no longer matters. */
		void testOpxAlgorithmDecidesTheCarrier (void)
		{
			Stream silent;
			startOpxFm (silent, 0, 0x05);
			writeOpx (silent, 0, 3, 0x4, 0, 0x7F);				// S4 at maximum attenuation
			writeOpx (silent, 100, 0, 0x0, 0, 0x01);			// key on

			Stream audible;
			startOpxFm (audible, 0, 0x08);
			writeOpx (audible, 0, 3, 0x4, 0, 0x7F);				// the same silenced S4
			writeOpx (audible, 100, 0, 0x0, 0, 0x01);

			check (runOpx (silent, 8000).empty(),
				   "algorithm 5 with a silenced carrier makes no note");
			check (runOpx (audible, 8000).size() == 1,
				   "algorithm 8 adds S1 beside the silenced S4");
		}

		/** Register 0xD is the group's two output levels: 13 and up are -96 dB in the core, so a
			muted group must not look like a sounding one. */
		void testOpxGroupLevelMutes (void)
		{
			Stream loud;
			startOpxFm (loud, 0, 0x08);
			writeOpx (loud, 0, 0, 0xD, 0, 0x00);				// both outputs at 0 dB
			writeOpx (loud, 100, 0, 0x0, 0, 0x01);

			Stream muted;
			startOpxFm (muted, 0, 0x08);
			writeOpx (muted, 0, 0, 0xD, 0, 0xDD);				// both outputs at -96 dB
			writeOpx (muted, 100, 0, 0x0, 0, 0x01);

			check (runOpx (loud, 4000).size() == 1, "a group at 0 dB sounds");
			check (runOpx (muted, 4000).empty(), "a group at -96 dB is not a note");
		}

		void testOpxBlockIsAnOctave (void)
		{
			Stream low;
			startOpxFm (low, 0, 0x08);
			writeOpx (low, 100, 0, 0x0, 0, 0x01);

			// 0xA only latches the high byte: the core recomputes fns and block on the next 0x9
			Stream high;
			startOpxFm (high, 0, 0x08);
			writeOpx (high, 0, 0, 0xA, 0, 0x55);				// block 5: same fns, one octave up
			writeOpx (high, 0, 0, 0x9, 0, 0x1C);
			writeOpx (high, 100, 0, 0x0, 0, 0x01);

			const auto lowNotes = runOpx (low, 4000);
			const auto highNotes = runOpx (high, 4000);

			check (! lowNotes.empty() && ! highNotes.empty(), "both octaves sound");
			if (lowNotes.empty() || highNotes.empty())
			{
				return;
			}

			check (highNotes[0].note == lowNotes[0].note + 12, "one block is one octave");
		}

		/** Sync mode 1 splits the group into banks 0/2 and 1/3. Under algorithm 0 the carrier is
			bank 2, which is never written directly - only the bank-0 key-on can ever sound it. */
		void testOpxSyncOneCopiesToThePair (void)
		{
			Stream stream;
			stream.at (0, 6, 0x00, 0x01);						// group 0: two 2-operator voices
			writeOpx (stream, 0, 0, 0x3, 0, 0x01);				// multiple 1, pair A slot 1
			writeOpx (stream, 0, 2, 0x3, 0, 0x01);				// multiple 1, pair A slot 2
			writeOpx (stream, 0, 0, 0xA, 0, 0x45);				// pitch, copied to bank 2
			writeOpx (stream, 0, 0, 0x9, 0, 0x1C);
			writeOpx (stream, 0, 0, 0xC, 0, 0x00);				// algorithm 0: S3 (bank 2) carries
			writeOpx (stream, 100, 0, 0x0, 0, 0x01);			// key on pair A

			const auto notes = runOpx (stream, 4000);

			check (notes.size() == 1, "a pair A key-on copies into bank 2");
			if (notes.empty())
			{
				return;
			}

			check (absoluteNote (notes[0]) == 69, "the pair's carrier plays A4");
			check (notes[0].logicalChannel == 0, "the pair reports on the group's channel");
		}

		/** Sync mode 3 makes the four banks four PCM slots. A sample has no pitch and no readable
			end, so it is a C4 marker closed by the decoder's own 80 ms gate - and the driver's key
			off arriving later changes nothing. */
		void testOpxPcmIsAOneShot (void)
		{
			Stream stream;
			stream.at (0, 6, 0x00, 0x03);						// group 0: four PCM slots
			writeOpx (stream, 100, 0, 0x0, 0, 0x01);			// key on bank 0
			writeOpx (stream, 5000, 0, 0x0, 0, 0x00);			// key off after the gate has closed

			const auto notes = runOpx (stream, 12000);

			check (notes.size() == 1, "a PCM key-on is one sample note");
			if (notes.empty())
			{
				return;
			}

			check (absoluteNote (notes[0]) == 60, "the sample is a C4 marker");
			check (notes[0].isDrum, "a PCM group is a drum track");
			check (notes[0].channelKind == toybox::chips::ChannelKind::adpcmDrum,
				   "the note carries the ADPCM family");
			check (distance (notes[0].startSample, 100) <= 64, "the note starts at the key-on");
			check (distance (notes[0].endSample, 3628) <= 64, "the 80 ms gate ends the sample");
		}

		/** In PCM mode the port is the bank and the address's low nibble the group - and the
			nibbles 3, 7, 11 and 15 have no group behind them. */
		void testOpxPcmBanksAreIndependent (void)
		{
			Stream stream;
			stream.at (0, 6, 0x00, 0x03);						// group 0: PCM
			stream.at (0, 6, 0x04, 0x03);						// nibble 4 is group 3: PCM as well
			writeOpx (stream, 100, 1, 0x0, 0x00, 0x01);			// bank 1, group 0
			writeOpx (stream, 200, 2, 0x0, 0x04, 0x01);			// bank 2, group 3

			const auto notes = runOpx (stream, 8000);

			check (notes.size() == 2, "two PCM slots key two notes");
			if (notes.size() != 2)
			{
				return;
			}

			check (notes[0].logicalChannel == 0 && notes[1].logicalChannel == 3,
				   "the port picks the bank, the low nibble the group, and 3/7/11/15 are holes");
		}
	}

	// --- C140: a playback rate, an end address and a loop bit ---------------------------------

	std::vector<NoteEvent> runC140 (const Stream& stream, std::uint32_t totalSamples,
									std::uint32_t clock = 8000000,
									const std::vector<std::uint8_t>& rom = {})
	{
		toybox::chips::C140Decoder decoder;
		decoder.reset (toybox::chips::ChipConfig { clock, 44100.0 });
		if (! rom.empty())
		{
			decoder.setRom (rom);
		}

		return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
	}

	/** A ROM holding one sine wave of `period` bytes - sixteen cycles of it - at 0x1000. */
	std::vector<std::uint8_t> makeC140SinRom (int period)
	{
		std::vector<std::uint8_t> rom (0x1100, 0);
		for (int i = 0; i < 16 * period; ++i)
		{
			const double phase = 2.0 * 3.14159265358979323846 * (double) i / (double) period;
			rom[0x1000 + (std::size_t) i] = (std::uint8_t) (128 + (int) std::lround (std::sin (phase) * 100.0));
		}

		return rom;
	}

	/** The voice the period tests key on: 256 bytes at 0x1000, looped back to the start, played at
		19931 - 8448 bytes a second at 8 MHz. */
	void setC140LoopedSampleAt (Stream& stream, std::uint16_t frequency)
	{
		stream.at (0, 0x00, 0x00, 0xFF);		// volume right
		stream.at (0, 0x00, 0x01, 0xFF);		// volume left
		stream.at (0, 0x00, 0x06, 0x10);		// start 0x1000
		stream.at (0, 0x00, 0x07, 0x00);
		stream.at (0, 0x00, 0x08, 0x11);		// end 0x1100: 256 bytes
		stream.at (0, 0x00, 0x09, 0x00);
		stream.at (0, 0x00, 0x0A, 0x10);		// loop back to the start
		stream.at (0, 0x00, 0x0B, 0x00);
		stream.at (0, 0x00, 0x02, (std::uint8_t) (frequency >> 8));		// the C140 holds it high first
		stream.at (0, 0x00, 0x03, (std::uint8_t) (frequency & 0xFF));
		stream.at (10, 0x00, 0x05, 0x90);		// key on with loop
		stream.at (8000, 0x00, 0x05, 0x10);		// the loop bit alone: the key-off
	}

	void setC140LoopedSample (Stream& stream)
	{
		stream.at (0, 0x00, 0x00, 0xFF);		// volume right
		stream.at (0, 0x00, 0x01, 0xFF);		// volume left
		stream.at (0, 0x00, 0x06, 0x10);		// start 0x1000
		stream.at (0, 0x00, 0x07, 0x00);
		stream.at (0, 0x00, 0x08, 0x11);		// end 0x1100: 256 bytes
		stream.at (0, 0x00, 0x09, 0x00);
		stream.at (0, 0x00, 0x0A, 0x10);		// loop back to the start
		stream.at (0, 0x00, 0x0B, 0x00);
		stream.at (0, 0x00, 0x02, 0x4D);		// frequency 19931
		stream.at (0, 0x00, 0x03, 0xDB);
		stream.at (10, 0x00, 0x05, 0x90);		// key on with loop
		stream.at (8000, 0x00, 0x05, 0x10);		// the loop bit alone: the key-off
	}

	/** Volume, a sample at 0x1000 with 1344 bytes, and the frequency for 13440 bytes a second. */
	void setC140Voice (Stream& stream, std::uint8_t port, std::uint8_t base)
	{
		stream.at (0, port, base, 0xFF);			// volume right
		stream.at (0, port, (std::uint8_t) (base + 1), 0xFF);	// volume left
		stream.at (0, port, (std::uint8_t) (base + 6), 0x10);	// start 0x1000
		stream.at (0, port, (std::uint8_t) (base + 7), 0x00);
		stream.at (0, port, (std::uint8_t) (base + 8), 0x15);	// end 0x1540
		stream.at (0, port, (std::uint8_t) (base + 9), 0x40);
		stream.at (0, port, (std::uint8_t) (base + 2), 0x7B);	// frequency 31711
		stream.at (0, port, (std::uint8_t) (base + 3), 0xDF);
	}

	/** The frequency register is a rate, not a note: at 8 MHz 31711 is 13440 bytes a second, and
		against the 8 kHz root rate that is A4. The sample is 1344 bytes, so the note lasts 100 ms -
		and no key-off is ever written, because the end address ends it. */
	void testC140Note (void)
	{
		Stream stream;
		setC140Voice (stream, 0x00, 0x00);
		stream.at (10, 0x05, 0x80);			// key on, no loop

		const auto notes = runC140 (stream, 20000);

		check (notes.size() == 1, "a C140 key-on is one note");
		if (notes.empty())
		{
			return;
		}

		check (absoluteNote (notes[0]) == 69, "31711 at 8 MHz is 13440 bytes a second, A4");
		check (notes[0].logicalChannel == 0, "C140 voice 0 is its own track");
		check (! notes[0].isDrum, "a sampled voice is a melodic track");
		check (distance (notes[0].endSample, 4420) <= 64, "the end address ends the note at 100 ms");
	}

	/** The length is the sample's own size, not a fixed window: half the bytes, half the note. */
	void testC140EndAddressSetsTheLength (void)
	{
		Stream stream;
		setC140Voice (stream, 0x00, 0x00);
		stream.at (0, 0x08, 0x12);			// end 0x12A0: 672 bytes
		stream.at (0, 0x09, 0xA0);
		stream.at (10, 0x05, 0x80);

		const auto notes = runC140 (stream, 20000);

		check (notes.size() == 1 && distance (notes[0].endSample, 2215) <= 64,
			   "672 bytes at 13440 a second is 50 ms");
	}

	/** start/end/loop are latched at key-on, where the core copies them into the voice: moving the
		end mid-note changes the next note, not the one playing. */
	void testC140EndAddressIsLatchedAtKeyOn (void)
	{
		Stream stream;
		setC140Voice (stream, 0x00, 0x00);
		stream.at (10, 0x05, 0x80);
		stream.at (1000, 0x08, 0x10);		// end 0x1080 mid-note: 128 bytes
		stream.at (1000, 0x09, 0x80);

		const auto notes = runC140 (stream, 20000);

		check (notes.size() == 1 && distance (notes[0].endSample, 4420) <= 64,
			   "the end address only takes effect on the next key-on");
	}

	/** With the loop bit the pointer wraps, so the end address never arrives and only a mode write
		without bit 7 - the driver's key-off - stops the voice. */
	void testC140LoopRunsUntilKeyOff (void)
	{
		Stream stream;
		setC140Voice (stream, 0x00, 0x00);
		stream.at (0, 0x0A, 0x10);			// loop back to the start
		stream.at (0, 0x0B, 0x00);
		stream.at (10, 0x05, 0x90);			// key on with loop
		stream.at (8000, 0x05, 0x10);		// loop bit only: the key-off

		const auto notes = runC140 (stream, 20000);

		check (notes.size() == 1 && distance (notes[0].endSample, 8000) <= 64,
			   "a looping voice runs past its end and only a key-off stops it");
	}

	/** The rate is the frequency times the clock, so doubling the clock is an octave. */
	void testC140ClockDoublesThePitch (void)
	{
		Stream stream;
		setC140Voice (stream, 0x00, 0x00);
		stream.at (10, 0x05, 0x80);

		const auto notes = runC140 (stream, 20000, 16000000);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 81, "double the clock is an octave up");
	}

	/** Silence is not a note: both volumes at zero attenuate the voice to nothing, and the core
		skips a voice whose frequency is zero (`if (frequency == 0) continue;`). */
	void testC140SilentVoiceIsNoNote (void)
	{
		Stream stream;
		stream.at (0, 0x06, 0x10);
		stream.at (0, 0x07, 0x00);
		stream.at (0, 0x08, 0x15);
		stream.at (0, 0x09, 0x40);
		stream.at (0, 0x02, 0x7B);
		stream.at (0, 0x03, 0xDF);
		stream.at (10, 0x05, 0x80);			// keyed, but the volumes stay at 0

		check (runC140 (stream, 20000).empty(), "a voice at zero volume is not a note");

		Stream zeroFrequency;
		zeroFrequency.at (0, 0x00, 0xFF);
		zeroFrequency.at (0, 0x01, 0xFF);
		zeroFrequency.at (0, 0x06, 0x10);
		zeroFrequency.at (0, 0x07, 0x00);
		zeroFrequency.at (0, 0x08, 0x15);
		zeroFrequency.at (0, 0x09, 0x40);
		zeroFrequency.at (10, 0x05, 0x80);	// no frequency written at all

		check (runC140 (zeroFrequency, 20000).empty(), "the core skips a zero-frequency voice");
	}

	/** Voices 16 and up live at offsets of 0x100 and above, so 0xD4's high offset byte is the only
		thing that says which voice a write belongs to; a decoder that dropped it would put every
		note on a low voice and still look right. */
	void testC140HighVoiceNeedsTheHighOffsetByte (void)
	{
		toybox::chips::C140Decoder decoder;
		check (decoder.channelCount() == 24, "the C140 has twenty-four voices");

		Stream stream;
		setC140Voice (stream, 0x01, 0x00);		// port 1, register 0: voice 16
		stream.at (0, 0x01, 0x02, 0xF7);		// frequency 63422, an octave above the helper's A4
		stream.at (0, 0x01, 0x03, 0xBE);
		stream.at (10, 0x01, 0x05, 0x80);

		const auto notes = runC140 (stream, 20000);

		check (notes.size() == 1, "a voice above 0xFF still keys on");
		if (notes.empty())
		{
			return;
		}

		check (notes[0].logicalChannel == 16, "offset 0x105 is voice 16, not voice 0");
		check (absoluteNote (notes[0]) == 81, "and it keeps its own frequency, an octave above voice 0's");
	}

	/** The cross-rate property, which is what makes the measured root trustworthy: the *same* sample
		at twice the rate must report the same root, and its transposition must move by exactly an
		octave. A root that moved with the rate would be the measurement reading the window instead
		of the sample. */
	void testC140RootIsStableAcrossRates (void)
	{
		const auto rom = makeC140SinRom (16);

		Stream octave;
		setC140LoopedSampleAt (octave, 39862);		// twice 19931
		const auto base = runC140 (octave, 20000, 8000000, rom);

		Stream half;
		setC140LoopedSampleAt (half, 19931);
		const auto lower = runC140 (half, 20000, 8000000, rom);

		check (base.size() == 1 && lower.size() == 1, "the same sample at two rates is two notes");
		if (base.empty() || lower.empty())
		{
			return;
		}

		check (base[0].rootCents == lower[0].rootCents,
			   "the same sample measures the same root at twice the rate");
		check (distance (base[0].transposeCents - lower[0].transposeCents, 1200) <= 4,
			   "and the transposition moves by exactly the octave the rate did");
	}

	/** The pitch is two layers, and this is the case that shows both: the note is the transposition
		the rate register asks for (8448 bytes a second against the 8 kHz reference is +94 cents, so
		C#4), while the sample's own root - a sixteen-byte cycle, which at 8 kHz would be 500 Hz - is
		a separate +1121 cents. Adding them puts the note back on C5, which is what the chip sounds,
		and that sum is the invariant the refactor is built on. */
	void testC140TranspositionAndRoot (void)
	{
		Stream stream;
		setC140LoopedSample (stream);

		const auto notes = runC140 (stream, 20000, 8000000, makeC140SinRom (16));

		check (notes.size() == 1, "one looped C140 voice is one note");
		if (notes.empty())
		{
			return;
		}

		check (notes[0].note == 61, "L1: 8448 bytes a second against 8 kHz is C#4");
		check (distance (notes[0].transposeCents, 94) <= 2, "L1 is +94 cents");
		check (distance (notes[0].rootCents, 1121) <= 3,
			   "L2: a sixteen-byte cycle at 8 kHz is 500 Hz, +1121 cents from middle C");
		check (notes[0].pitchSource == toybox::chips::PitchSource::sampleInferred,
			   "a measured root says so");
		check (distance (notes[0].note * 100 + notes[0].rootCents, 7200) <= 25,
			   "L1 + L2 is the C5 the chip sounds: the two layers must add up to the old mapping");
	}

	/** Without a ROM nothing can be measured, so the root rate still decides - that is the C140's
		behaviour before the data blocks reach the decoder, and it stays as the fallback. */
	void testC140WithoutRomKeepsTheRootRate (void)
	{
		Stream stream;
		setC140LoopedSample (stream);

		const auto notes = runC140 (stream, 20000);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 61,
			   "8448 bytes a second against the 8 kHz root is C#4 without a ROM");
	}

	/** The bank register is folded into the ROM, not appended to it. A System 2 voice is bank 0x14
		with address 0x95C0, which is ROM offset 0x495C0 - not 0x1495C0, which a one-megabyte ROM
		does not have. Getting this wrong makes every sample unmeasurable and the song falls back
		to the root rate, which is hard to tell from "the ROM is not wired up at all". */
	void testC140System2BankIsFolded (void)
	{
		std::vector<std::uint8_t> rom (0x100000, 0);
		const std::uint32_t offset = 0x495C0;		// (0x14 << 16 | 0x95C0) folded by System 2
		for (int i = 0; i < 256; ++i)
		{
			const double phase = 2.0 * 3.14159265358979323846 * (double) i / 16.0;
			rom[offset + (std::size_t) i] = (std::uint8_t) (128 + (int) std::lround (std::sin (phase) * 100.0));
		}

		Stream stream;
		stream.at (0, 0x00, 0x00, 0xFF);
		stream.at (0, 0x00, 0x01, 0xFF);
		stream.at (0, 0x00, 0x04, 0x14);		// bank 0x14
		stream.at (0, 0x00, 0x06, 0x95);		// start 0x95C0
		stream.at (0, 0x00, 0x07, 0xC0);
		stream.at (0, 0x00, 0x08, 0x96);		// end 0x96C0
		stream.at (0, 0x00, 0x09, 0xC0);
		stream.at (0, 0x00, 0x0A, 0x95);		// loop back to the start
		stream.at (0, 0x00, 0x0B, 0xC0);
		stream.at (0, 0x00, 0x02, 0x4D);		// the same 8448 bytes a second
		stream.at (0, 0x00, 0x03, 0xDB);
		stream.at (10, 0x00, 0x05, 0x90);
		stream.at (8000, 0x00, 0x05, 0x10);

		// the note is L1 and does not depend on the ROM at all, so the folding shows up in the root:
		// bank 0x14 address 0x95C0 is ROM offset 0x495C0, where the sixteen-byte cycle is measured
		const auto folded = runC140 (stream, 20000, 8000000, rom);
		check (folded.size() == 1 && folded[0].note == 61,
			   "the note is the transposition, C#4, whatever the ROM holds");
		check (! folded.empty() && distance (folded[0].rootCents, 1121) <= 3,
			   "bank 0x14 address 0x95C0 is ROM offset 0x495C0, where the root is measured");

		// the same registers with a linear ROM (the banking type the header may name instead)
		// address 0x1495C0, which no one-megabyte image holds: no root is measured, so L2 is zero
		toybox::chips::C140Decoder decoder;
		decoder.reset (toybox::chips::ChipConfig { 8000000, 44100.0 });
		decoder.setRom (rom, toybox::chips::C140Decoder::Banking::linear);
		const auto linear = toybox::chips::extractNotes (decoder, stream.writes, 20000);
		check (linear.size() == 1 && absoluteNote (linear[0]) == 61 && linear[0].rootCents == 0,
			   "an unfolded address past the ROM leaves no root, so only L1 remains");
	}

	/** A sample that never repeats - a drum, noise - must not get an invented pitch: its bytes
		correlate with nothing, so the root rate decides there too. */
	void testC140NoisySampleFallsBackToTheRootRate (void)
	{
		std::vector<std::uint8_t> rom (0x1100, 0);
		std::uint32_t seed = 12345u;
		for (int i = 0; i < 256; ++i)
		{
			seed = seed * 1103515245u + 12345u;
			rom[0x1000 + (std::size_t) i] = (std::uint8_t) (seed >> 16);
		}

		Stream stream;
		setC140LoopedSample (stream);

		const auto notes = runC140 (stream, 20000, 8000000, rom);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 61,
			   "a sample with no period keeps the root rate");
	}

	// --- C352: a playback rate behind an explicit key-on write -----------------------------------

	constexpr std::uint16_t kC352KeyOn = 0x4000;
	constexpr std::uint16_t kC352KeyOff = 0x2000;
	constexpr std::uint16_t kC352Loop = 0x0002;

	std::vector<NoteEvent> runC352 (const Stream& stream, std::uint32_t totalSamples,
									std::uint32_t clock = 12288000,
									const std::vector<std::uint8_t>& rom = {})
	{
		toybox::chips::C352Decoder decoder;
		decoder.reset (toybox::chips::ChipConfig { clock, 44100.0 });
		if (! rom.empty())
		{
			decoder.setRom (rom);
		}

		return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
	}

	/** One C352 register write: the VGM's 0xE1 carries a 16-bit big-endian offset and a 16-bit
		big-endian value, so the offset's high byte goes in `port` and the value is one 16-bit word. */
	void c352Write (Stream& stream, std::uint32_t time, std::uint16_t offset, std::uint16_t value)
	{
		RegisterWrite write;
		write.port = (std::uint8_t) ((offset >> 8) & 0x7F);
		write.reg = (std::uint8_t) (offset & 0xFF);
		write.value = value;
		write.sampleTime = time;
		stream.writes.push_back (write);
	}

	/** Volume on all four outputs, a sample from `start` to `end`, the loop register, the frequency
		and the flags - everything but the execute write, which is what actually keys the voice on. */
	void setC352Voice (Stream& stream, int voice, std::uint16_t start, std::uint16_t end,
					   std::uint16_t loop, std::uint16_t frequency, std::uint16_t flags)
	{
		const std::uint16_t base = (std::uint16_t) (voice * 8);
		c352Write (stream, 0, (std::uint16_t) (base + 0), 0xFFFF);		// front left/right
		c352Write (stream, 0, (std::uint16_t) (base + 1), 0xFFFF);		// rear left/right
		c352Write (stream, 0, (std::uint16_t) (base + 2), frequency);
		c352Write (stream, 0, (std::uint16_t) (base + 4), 0x0000);		// bank 0
		c352Write (stream, 0, (std::uint16_t) (base + 5), start);
		c352Write (stream, 0, (std::uint16_t) (base + 6), end);
		c352Write (stream, 0, (std::uint16_t) (base + 7), loop);
		c352Write (stream, 0, (std::uint16_t) (base + 3), flags);
	}

	/** The execute write at offset 0x202: it starts every voice whose flag register asks for it. */
	void c352Execute (Stream& stream, std::uint32_t time)
	{
		c352Write (stream, time, 0x0202, 0x0000);
	}

	/** `bytes` of a two-level waveform at `offset`, one cycle every `period` bytes. */
	void fillC352Square (std::vector<std::uint8_t>& rom, std::size_t offset, int period,
						 std::size_t bytes)
	{
		for (std::size_t i = 0; i < bytes; ++i)
		{
			const bool high = (i % (std::size_t) period) < (std::size_t) period / 2;
			rom[offset + i] = high ? 0x50 : 0xB0;		// +80 / -80 as signed 8-bit PCM
		}
	}

	/** The frequency that plays 28160 bytes a second at 12.288 MHz: the rate a 64-byte cycle turns
		into 440 Hz. It is the rate the root-rate tests use, so their notes land where the root
		puts them rather than on a rounding step. */
	constexpr std::uint16_t kC352Rate28160 = 43254;

	/** At 12.288 MHz the frequency register is a rate of `frequency * 12288000 / 18874368` bytes a
		second, and 0x8000 is exactly the chip's own rate (`clock / 576`) - middle C. The note also
		needs both bytes of the register to arrive: a decoder that kept only the low byte would read
		zero and stay silent, and one that read the execute write as offset 0x02 would clear it. */
	void testC352Note (void)
	{
		Stream stream;
		setC352Voice (stream, 0, 0x0000, 0x0F00, 0x0000, 0x8000, kC352KeyOn);
		c352Execute (stream, 10);

		const auto notes = runC352 (stream, 20000);

		check (notes.size() == 1, "a C352 key-on is one note");
		if (notes.empty())
		{
			return;
		}

		check (absoluteNote (notes[0]) == 60, "0x8000 at 12.288 MHz is 21333 bytes a second, middle C");
		check (notes[0].logicalChannel == 0, "C352 voice 0 is its own track");
		check (! notes[0].isDrum, "a sampled voice is a melodic track");
		check (distance (notes[0].startSample, 10) <= 64, "the note starts at the execute write");
		check (distance (notes[0].endSample, 7948) <= 64,
			   "3840 bytes at 21333 a second is 180 ms, with no key-off written");
	}

	/** The flag write is not the key-on: the core only starts a voice when offset 0x202 is written,
		so a decoder that keyed on the flag would start every note a command early - and one that
		ignored 0x202 would never start any. */
	void testC352KeyOnIsTheExecuteWrite (void)
	{
		Stream stream;
		setC352Voice (stream, 0, 0x0000, 0x0F00, 0x0000, 0x8000, kC352KeyOn);

		check (runC352 (stream, 20000).empty(), "the key-on flag alone starts nothing");

		c352Execute (stream, 1000);
		const auto notes = runC352 (stream, 20000);

		check (notes.size() == 1 && distance (notes[0].startSample, 1000) <= 64,
			   "the execute write is where the note begins");
	}

	/** The length is the sample's own size: half the bytes, half the note. */
	void testC352EndAddressSetsTheLength (void)
	{
		Stream stream;
		setC352Voice (stream, 0, 0x0000, 0x0780, 0x0000, 0x8000, kC352KeyOn);
		c352Execute (stream, 10);

		const auto notes = runC352 (stream, 20000);

		check (notes.size() == 1 && distance (notes[0].endSample, 3979) <= 64,
			   "1920 bytes at 21333 a second is 90 ms");
	}

	/** With the loop bit the pointer jumps back to the loop address, so the end address never
		arrives and only the driver's key-off - its flag plus the execute write - stops the voice. */
	void testC352KeyOffEndsALoopingVoice (void)
	{
		Stream stream;
		setC352Voice (stream, 0, 0x0000, 0x0400, 0x0000, 0x8000, (std::uint16_t) (kC352KeyOn | kC352Loop));
		c352Execute (stream, 10);
		c352Write (stream, 8000, 0x0003, (std::uint16_t) (kC352KeyOff | kC352Loop));
		c352Execute (stream, 8000);

		const auto notes = runC352 (stream, 20000);

		check (notes.size() == 1 && distance (notes[0].endSample, 8000) <= 64,
			   "a looping voice runs past its end and only a key-off stops it");
	}

	/** Silence is not a note: zero on all four outputs attenuates the voice to nothing, and a
		frequency of zero never carries the counter over, so the core fetches no byte at all. */
	void testC352SilentVoiceIsNoNote (void)
	{
		Stream stream;
		c352Write (stream, 0, 0x0002, 0x8000);		// frequency, but the volumes stay 0
		c352Write (stream, 0, 0x0006, 0x0F00);
		c352Write (stream, 0, 0x0003, kC352KeyOn);
		c352Execute (stream, 10);

		check (runC352 (stream, 20000).empty(), "a voice at zero volume is not a note");

		Stream noFrequency;
		setC352Voice (noFrequency, 0, 0x0000, 0x0F00, 0x0000, 0x0000, kC352KeyOn);
		c352Execute (noFrequency, 10);

		check (runC352 (noFrequency, 20000).empty(), "a voice with no frequency is silent");
	}

	/** Every voice register is a whole word, and the two halves of one arrive in one command - so
		the volume pair and the frequency are only right when both bytes are used. The keyed bit
		here sits in the high half of the flag word, which a low-byte-only decoder would miss. */
	void testC352RegistersAreSixteenBits (void)
	{
		Stream stream;
		c352Write (stream, 0, 0x0000, 0xFF00);		// front left only: the high half of vol_f
		c352Write (stream, 0, 0x0001, 0x00FF);		// rear right only: the low half of vol_r
		c352Write (stream, 0, 0x0002, 0x8000);
		c352Write (stream, 0, 0x0006, 0x0F00);
		c352Write (stream, 0, 0x0003, kC352KeyOn);
		c352Execute (stream, 10);

		const auto notes = runC352 (stream, 20000);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 60,
			   "one output channel at full volume is a note, and the volume pairs are words");
	}

	/** With the ROM, the note is the sample's own pitch: 28160 bytes a second over a 64-byte cycle
		is 440 Hz, A4 - not the 65 the root rate claims for the same rate. */
	void testC352SamplePeriodIsTheNote (void)
	{
		std::vector<std::uint8_t> rom (0x2000, 0);
		fillC352Square (rom, 0x0000, 64, 2816);

		Stream stream;
		setC352Voice (stream, 0, 0x0000, 2816, 0x0000, kC352Rate28160, kC352KeyOn);
		c352Execute (stream, 10);

		const auto notes = runC352 (stream, 20000, 12288000, rom);

		check (notes.size() == 1, "one non-looping C352 voice is one note");
		if (notes.empty())
		{
			return;
		}

		check (absoluteNote (notes[0]) == 69, "64 bytes a cycle at 28160 a second is 440 Hz, A4");
		// 28160 against the chip's own 12288000/576 = 21333 is +481 cents, so F4 - the absolute
		// A4 only appears once the sample's root (a 64-byte cycle, +420 cents) is added
		check (notes[0].note == 65 && distance (notes[0].transposeCents, 481) <= 3,
			   "L1 alone is F4 with +481 cents");
	}

	/** Without a ROM nothing can be measured, so the root rate still decides: 28160 bytes a second
		against the chip's own 21333 is a fourth up, MIDI 65. */
	void testC352WithoutRomKeepsTheRootRate (void)
	{
		Stream stream;
		setC352Voice (stream, 0, 0x0000, 2816, 0x0000, kC352Rate28160, kC352KeyOn);
		c352Execute (stream, 10);

		const auto notes = runC352 (stream, 20000);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 65,
			   "28160 bytes a second against the clock/576 root is F4 without a ROM");
	}

	/** A sample that never repeats - a drum, noise - must not get an invented pitch: its bytes
		correlate with nothing, so the root rate decides there too. */
	void testC352NoisySampleFallsBackToTheRootRate (void)
	{
		std::vector<std::uint8_t> rom (0x2000, 0);
		std::uint32_t seed = 12345u;
		for (int i = 0; i < 2816; ++i)
		{
			seed = seed * 1103515245u + 12345u;
			rom[0x0000 + (std::size_t) i] = (std::uint8_t) (seed >> 16);
		}

		Stream stream;
		setC352Voice (stream, 0, 0x0000, 2816, 0x0000, kC352Rate28160, kC352KeyOn);
		c352Execute (stream, 10);

		const auto notes = runC352 (stream, 20000, 12288000, rom);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 65,
			   "a sample with no period keeps the root rate");
	}

	/** A sample address is `(bank << 16) | start`, and the core masks it with the ROM's size - so
		the same start offset in bank 1 is a different sample, 64 KB further in. A decoder that
		dropped the bank would measure the wrong waveforms and send the same note twice. */
	void testC352BankIsPartOfTheAddress (void)
	{
		std::vector<std::uint8_t> rom (0x20000, 0);
		fillC352Square (rom, 0x0000, 64, 2816);			// bank 0: 440 Hz at this rate
		fillC352Square (rom, 0x10000, 32, 2816);		// bank 1: an octave up

		Stream low;
		setC352Voice (low, 0, 0x0000, 2816, 0x0000, kC352Rate28160, kC352KeyOn);
		c352Execute (low, 10);

		Stream high;
		c352Write (high, 0, 0x0000, 0xFFFF);
		c352Write (high, 0, 0x0001, 0xFFFF);
		c352Write (high, 0, 0x0002, kC352Rate28160);
		c352Write (high, 0, 0x0004, 0x0001);			// bank 1
		c352Write (high, 0, 0x0005, 0x0000);
		c352Write (high, 0, 0x0006, 2816);
		c352Write (high, 0, 0x0003, kC352KeyOn);
		c352Execute (high, 10);

		const auto lowNotes = runC352 (low, 20000, 12288000, rom);
		const auto highNotes = runC352 (high, 20000, 12288000, rom);

		check (lowNotes.size() == 1 && absoluteNote (lowNotes[0]) == 69,
			   "bank 0 start 0 is the 64-byte cycle, A4");
		check (highNotes.size() == 1 && absoluteNote (highNotes[0]) == 81,
			   "bank 1 start 0 is 64 KB further in, and an octave up");
	}

	/** Some of Namco's own drivers play a whole song from one sixteen-byte waveform: the sample *is*
		one cycle, it loops from its first byte, and the frequency register does the rest. Sixteen
		bytes is shorter than any correlation window, so the loop length has to be the cycle -
		without that the voice is quantised against the root rate and lands more than three octaves
		low, which is exactly what the Galaga rips in `test_file/c352` did. The end address is
		inclusive (the core fetches the byte at `wave_end` and only then checks for the end), so the
		same waveform that stops one byte earlier is a semitone higher. */
	void testC352WavetableCycle (void)
	{
		std::vector<std::uint8_t> rom (0x2000, 0);
		for (int i = 0; i < 16; ++i)
		{
			const double value = std::sin ((double) i * 2.0 * 3.14159265358979 / 16.0) * 100.0;
			rom[0x1000 + (std::size_t) i] = (std::uint8_t) (std::int8_t) std::lround (value);
		}

		// 7040 bytes a second over a sixteen-byte cycle is 440 Hz, A4
		constexpr std::uint16_t kRate7040 = 10813;

		Stream whole;
		setC352Voice (whole, 0, 0x1000, 0x100F, 0x1000, kRate7040,
					  (std::uint16_t) (kC352KeyOn | kC352Loop));
		c352Execute (whole, 10);

		Stream shortCycle;
		setC352Voice (shortCycle, 0, 0x1000, 0x100E, 0x1000, kRate7040,
					  (std::uint16_t) (kC352KeyOn | kC352Loop));
		c352Execute (shortCycle, 10);

		const auto wholeNotes = runC352 (whole, 20000, 12288000, rom);
		const auto shortNotes = runC352 (shortCycle, 20000, 12288000, rom);

		check (wholeNotes.size() == 1 && absoluteNote (wholeNotes[0]) == 69,
			   "a one-cycle wavetable loop of sixteen bytes at 7040 a second is A4");
		check (shortNotes.size() == 1 && absoluteNote (shortNotes[0]) == 70,
			   "the byte at the end address is part of the sample: fifteen bytes is a semitone up");
	}

	/** Thirty-two voices, and the voice number is the offset divided by eight - so the high voices
		only exist when the whole 16-bit offset is reassembled. */
	void testC352HighVoices (void)
	{
		toybox::chips::C352Decoder decoder;
		check (decoder.channelCount() == 32, "the C352 has thirty-two voices");

		Stream stream;
		setC352Voice (stream, 16, 0x0000, 0x0F00, 0x0000, 0x8000, kC352KeyOn);
		setC352Voice (stream, 31, 0x0000, 0x0F00, 0x0000, 0x4000, kC352KeyOn);
		c352Execute (stream, 10);

		const auto notes = runC352 (stream, 20000);

		check (notes.size() == 2, "both high voices key on");
		if (notes.size() != 2)
		{
			return;
		}

		check (notes[0].logicalChannel == 16 && absoluteNote (notes[0]) == 60,
			   "offset 0x080 is voice 16, at middle C");
		check (notes[1].logicalChannel == 31 && absoluteNote (notes[1]) == 48,
			   "offset 0x0F8 is voice 31, an octave below");
	}

	// --- K053260: four voices keyed by a shared mask register ------------------------------------

	std::vector<NoteEvent> runK053260 (const Stream& stream, std::uint32_t totalSamples,
									   std::uint32_t clock = 3579545,
									   const std::vector<std::uint8_t>& rom = {})
	{
		toybox::chips::K053260Decoder decoder;
		decoder.reset (toybox::chips::ChipConfig { clock, 44100.0 });
		if (! rom.empty())
		{
			decoder.setRom (rom);
		}

		return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
	}

	void k053260Write (Stream& stream, std::uint32_t time, std::uint8_t offset, std::uint8_t value)
	{
		RegisterWrite write;
		write.reg = offset;
		write.value = value;
		write.sampleTime = time;
		stream.writes.push_back (write);
	}

	/** Sound output on, both pan registers at centre (4), then the voice's four registers. The
		start address is the one the driver writes: the core's first fetch is `start + 1`. */
	void setK053260Voice (Stream& stream, int voice, std::uint32_t start, std::uint16_t length,
						  std::uint16_t pitch, std::uint8_t volume)
	{
		const std::uint8_t base = (std::uint8_t) (0x08 + voice * 8);
		k053260Write (stream, 0, 0x2F, 0x06);		// control: sound output enabled
		k053260Write (stream, 0, 0x2C, 0x24);		// pan voices 0/1: both centre
		k053260Write (stream, 0, 0x2D, 0x24);		// pan voices 2/3: both centre
		k053260Write (stream, 0, (std::uint8_t) (base + 0), (std::uint8_t) (pitch & 0xFF));
		k053260Write (stream, 0, (std::uint8_t) (base + 1), (std::uint8_t) (pitch >> 8));
		k053260Write (stream, 0, (std::uint8_t) (base + 2), (std::uint8_t) (length & 0xFF));
		k053260Write (stream, 0, (std::uint8_t) (base + 3), (std::uint8_t) (length >> 8));
		k053260Write (stream, 0, (std::uint8_t) (base + 4), (std::uint8_t) (start & 0xFF));
		k053260Write (stream, 0, (std::uint8_t) (base + 5), (std::uint8_t) ((start >> 8) & 0xFF));
		k053260Write (stream, 0, (std::uint8_t) (base + 6), (std::uint8_t) ((start >> 16) & 0x1F));
		k053260Write (stream, 0, (std::uint8_t) (base + 7), volume);
	}

	/** The key mask: a rising bit starts that voice, a cleared bit stops it. */
	void k053260Key (Stream& stream, std::uint32_t time, std::uint8_t mask)
	{
		k053260Write (stream, time, 0x28, mask);
	}

	void fillK053260Square (std::vector<std::uint8_t>& rom, std::size_t offset, int period,
							std::size_t bytes)
	{
		for (std::size_t i = 0; i < bytes; ++i)
		{
			const bool high = (i % (std::size_t) period) < (std::size_t) period / 2;
			rom[offset + i] = high ? 0x50 : 0xB0;
		}
	}

	/** At 3.579545 MHz a voice advances by `pitch * clock / 262144` bytes a second, so pitch 2062
		is 28156 bytes a second. A 64-byte cycle is 440 Hz - A4 - and the length register says the
		note lasts 1024 bytes, which is 36 ms and needs no key-off. */
	void testK053260Note (void)
	{
		std::vector<std::uint8_t> rom (0x2000, 0);
		fillK053260Square (rom, 0x1000, 64, 1024);

		Stream stream;
		setK053260Voice (stream, 0, 0x0FFF, 1024, 2062, 0x7F);
		k053260Key (stream, 10, 0x01);

		const auto notes = runK053260 (stream, 20000, 3579545, rom);

		check (notes.size() == 1, "a K053260 key-on is one note");
		if (notes.empty())
		{
			return;
		}

		check (absoluteNote (notes[0]) == 69, "a 64-byte cycle at 28156 bytes a second is A4");
		check (notes[0].logicalChannel == 0, "K053260 voice 0 is its own track");
		check (! notes[0].isDrum, "a sample with a period is melodic");
		check (distance (notes[0].startSample, 10) <= 64, "the note starts at the key mask write");
		check (distance (notes[0].endSample, 1614) <= 64,
			   "the length register ends the note: 1024 bytes at 28156 a second");
	}

	/** The mask is a level with an edge: writing the same bit again starts nothing, clearing it
		stops the voice, and raising it again is the next note. */
	void testK053260KeyOnIsARisingEdge (void)
	{
		std::vector<std::uint8_t> rom (0x2000, 0);
		fillK053260Square (rom, 0x1000, 64, 1024);

		Stream stream;
		setK053260Voice (stream, 0, 0x0FFF, 1024, 2062, 0x7F);
		k053260Key (stream, 10, 0x01);
		k053260Key (stream, 2000, 0x01);		// same level: not a new note
		k053260Key (stream, 4000, 0x00);		// key off
		k053260Key (stream, 6000, 0x01);		// and on again

		const auto notes = runK053260 (stream, 20000, 3579545, rom);

		check (notes.size() == 2, "a repeated key mask bit is not a second key-on");
		if (notes.size() != 2)
		{
			return;
		}

		check (distance (notes[1].startSample, 6000) <= 64,
			   "the note that follows a key-off starts at the next rising edge");
	}

	/** Half the length register, half the note - the length is the sample's own size, not a guess. */
	void testK053260LengthSetsTheDuration (void)
	{
		std::vector<std::uint8_t> rom (0x2000, 0);
		fillK053260Square (rom, 0x1000, 64, 1024);

		Stream stream;
		setK053260Voice (stream, 0, 0x0FFF, 512, 2062, 0x7F);
		k053260Key (stream, 10, 0x01);

		const auto notes = runK053260 (stream, 20000, 3579545, rom);

		check (notes.size() == 1 && distance (notes[0].endSample, 812) <= 64,
			   "512 bytes at 28156 a second is 18 ms");
	}

	/** With the loop bit the pointer wraps instead of stopping, so only a key-off ends it. */
	void testK053260LoopRunsUntilKeyOff (void)
	{
		std::vector<std::uint8_t> rom (0x2000, 0);
		fillK053260Square (rom, 0x1000, 64, 1024);

		Stream stream;
		setK053260Voice (stream, 0, 0x0FFF, 1024, 2062, 0x7F);
		k053260Write (stream, 0, 0x2A, 0x01);		// loop voice 0
		k053260Key (stream, 10, 0x01);
		k053260Key (stream, 8000, 0x00);

		const auto notes = runK053260 (stream, 20000, 3579545, rom);

		check (notes.size() == 1 && distance (notes[0].endSample, 8000) <= 64,
			   "a looping voice runs past its length and only a key-off stops it");
	}

	/** Silence is not a note: no volume, a pan of 0, a pitch of 0 and a length of 0 each leave the
		voice with nothing to play, and the control register's sound bit gates the whole chip. */
	void testK053260SilentVoiceIsNoNote (void)
	{
		std::vector<std::uint8_t> rom (0x2000, 0);
		fillK053260Square (rom, 0x1000, 64, 1024);

		Stream quiet;
		setK053260Voice (quiet, 0, 0x0FFF, 1024, 2062, 0x00);
		k053260Key (quiet, 10, 0x01);
		check (runK053260 (quiet, 20000, 3579545, rom).empty(), "a voice at zero volume is not a note");

		Stream unplugged;
		setK053260Voice (unplugged, 0, 0x0FFF, 1024, 2062, 0x7F);
		k053260Write (unplugged, 0, 0x2C, 0x20);		// voice 0 pan 0, voice 1 centre
		k053260Key (unplugged, 10, 0x01);
		check (runK053260 (unplugged, 20000, 3579545, rom).empty(), "a voice panned out is silent");

		Stream noPitch;
		setK053260Voice (noPitch, 0, 0x0FFF, 1024, 0x0000, 0x7F);
		k053260Key (noPitch, 10, 0x01);
		check (runK053260 (noPitch, 20000, 3579545, rom).empty(), "a pitch of 0 never fetches a byte");

		Stream noLength;
		setK053260Voice (noLength, 0, 0x0FFF, 0, 2062, 0x7F);
		k053260Key (noLength, 10, 0x01);
		check (runK053260 (noLength, 20000, 3579545, rom).empty(), "a length of 0 has no sample");

		Stream noSound;
		setK053260Voice (noSound, 0, 0x0FFF, 1024, 2062, 0x7F);
		k053260Write (noSound, 0, 0x2F, 0x04);		// control: sound output off
		k053260Key (noSound, 10, 0x01);
		check (runK053260 (noSound, 20000, 3579545, rom).empty(),
			   "the control register's sound bit gates the chip, as the core does");
	}

	/** A drum has no cycle to measure - a kick sweeps, a snare is noise - and reporting a pitch for
		it would make a DAW play a tone. It comes out as the C4 trigger with the drum family, which
		is what the four shared voices need on these boards. */
	void testK053260PercussionIsATrigger (void)
	{
		std::vector<std::uint8_t> rom (0x2000, 0);
		std::uint32_t seed = 987654321u;
		for (int i = 0; i < 1024; ++i)
		{
			seed = seed * 1103515245u + 12345u;
			rom[0x1000 + (std::size_t) i] = (std::uint8_t) (seed >> 16);
		}

		Stream stream;
		setK053260Voice (stream, 0, 0x0FFF, 1024, 2062, 0x7F);
		k053260Key (stream, 10, 0x01);

		const auto notes = runK053260 (stream, 20000, 3579545, rom);

		check (notes.size() == 1, "a percussion voice keyed on is one hit");
		if (notes.empty())
		{
			return;
		}

		check (absoluteNote (notes[0]) == 60, "a sample with no period is the C4 trigger, not a pitch");
		check (notes[0].isDrum, "and it is marked as a drum");
		check (notes[0].channelKind == toybox::chips::ChannelKind::adpcmDrum,
			   "its family says the pitch is not real");
	}

	/** A drum's bytes can repeat at a tiny lag - an alternating byte pair every two samples - and at
		this chip's rates that "period" reads as several kilohertz. Nothing on these boards is written
		up there, so a measured pitch above C8 (108) is a measurement artifact and the hit stays a
		trigger; a real note below the ceiling is unaffected. */
	void testK053260UnmusicalPeriodIsATrigger (void)
	{
		std::vector<std::uint8_t> rom (0x2000, 0);
		fillK053260Square (rom, 0x1000, 8, 1024);

		Stream fast;
		setK053260Voice (fast, 0, 0x0FFF, 1024, 3000, 0x7F);		// 40965 bytes/s over 8 = 5.1 kHz
		k053260Key (fast, 10, 0x01);

		const auto notes = runK053260 (fast, 20000, 3579545, rom);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 60 && notes[0].isDrum,
			   "a period that lands above C8 is a drum hit, not a 5 kHz note");
	}

	/** KADPCM is a stream of deltas: the nibbles are integrated before the period is measured, and
		low nibble first. Sixteen nibbles of +1 then -1 make a triangle, so 128 nibbles hold eight
		cycles and a pitch of 516 (7046 nibbles a second) is A4 again. */
	void testK053260KadpcmIsMeasured (void)
	{
		std::vector<std::uint8_t> rom (0x2000, 0);
		const std::uint8_t pattern[8] = { 0x11, 0x11, 0x11, 0x11, 0xFF, 0xFF, 0xFF, 0xFF };
		for (std::size_t i = 0; i < 64; ++i)
		{
			rom[0x1000 + i] = pattern[i % 8];
		}

		Stream stream;
		setK053260Voice (stream, 0, 0x0FFF, 64, 516, 0x7F);
		k053260Write (stream, 0, 0x2A, 0x10);		// voice 0: KADPCM
		k053260Key (stream, 10, 0x01);

		const auto notes = runK053260 (stream, 20000, 3579545, rom);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 69,
			   "a decoded KADPCM triangle of sixteen nibbles at 7046 a second is A4");
	}

	/** Four voices, and the last one lives at offset 0x20 with its pan in the second pan register. */
	void testK053260Voices (void)
	{
		toybox::chips::K053260Decoder decoder;
		check (decoder.channelCount() == 4, "the K053260 has four voices");

		std::vector<std::uint8_t> rom (0x2000, 0);
		fillK053260Square (rom, 0x1000, 64, 1024);

		Stream stream;
		setK053260Voice (stream, 3, 0x0FFF, 1024, 2062, 0x7F);
		k053260Key (stream, 10, 0x08);

		const auto notes = runK053260 (stream, 20000, 3579545, rom);

		check (notes.size() == 1 && notes[0].logicalChannel == 3,
			   "offset 0x20 with key mask bit 3 is voice 3, at A4");
	}

	// --- K054539: an attenuating volume and a sample that ends on a marker ------------------------

	std::vector<NoteEvent> runK054539 (const Stream& stream, std::uint32_t totalSamples,
									   std::uint32_t clock = 18432000,
									   const std::vector<std::uint8_t>& rom = {})
	{
		toybox::chips::K054539Decoder decoder;
		decoder.reset (toybox::chips::ChipConfig { clock, 44100.0 });
		if (! rom.empty())
		{
			decoder.setRom (rom);
		}

		return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
	}

	/** The chip is addressed with sixteen bits (0xD3), so the high byte of the offset travels in
		the write's port - and its key-on/key-off registers (0x214/0x215) are not channel indices. */
	void k054539Write (Stream& stream, std::uint32_t time, std::uint16_t offset, std::uint8_t value)
	{
		RegisterWrite write;
		write.port = (std::uint8_t) ((offset >> 8) & 0xFF);
		write.reg = (std::uint8_t) (offset & 0xFF);
		write.value = value;
		write.sampleTime = time;
		stream.writes.push_back (write);
	}

	const std::uint8_t k054539Type8 = 0x00;
	const std::uint8_t k054539Type16 = 0x04;
	const std::uint8_t k054539Type4 = 0x08;

	/** One voice: output on, format, no loop, pitch, volume, pan centre and the start address. */
	void setK054539Voice (Stream& stream, int channel, std::uint32_t start, std::uint8_t type,
						  std::uint32_t pitch, std::uint8_t volume)
	{
		const std::uint16_t base = (std::uint16_t) (channel * 0x20);
		k054539Write (stream, 0, 0x22F, 0x01);		// control: PCM output enabled
		k054539Write (stream, 0, (std::uint16_t) (0x200 + 2 * channel), type);
		k054539Write (stream, 0, (std::uint16_t) (0x201 + 2 * channel), 0x00);		// no loop
		k054539Write (stream, 0, (std::uint16_t) (base + 0x00), (std::uint8_t) (pitch & 0xFF));
		k054539Write (stream, 0, (std::uint16_t) (base + 0x01), (std::uint8_t) ((pitch >> 8) & 0xFF));
		k054539Write (stream, 0, (std::uint16_t) (base + 0x02), (std::uint8_t) ((pitch >> 16) & 0xFF));
		k054539Write (stream, 0, (std::uint16_t) (base + 0x03), volume);
		k054539Write (stream, 0, (std::uint16_t) (base + 0x05), 0x10);				// pan centre
		k054539Write (stream, 0, (std::uint16_t) (base + 0x08), 0x00);
		k054539Write (stream, 0, (std::uint16_t) (base + 0x09), 0x00);
		k054539Write (stream, 0, (std::uint16_t) (base + 0x0A), 0x00);
		k054539Write (stream, 0, (std::uint16_t) (base + 0x0C), (std::uint8_t) (start & 0xFF));
		k054539Write (stream, 0, (std::uint16_t) (base + 0x0D), (std::uint8_t) ((start >> 8) & 0xFF));
		k054539Write (stream, 0, (std::uint16_t) (base + 0x0E), (std::uint8_t) ((start >> 16) & 0xFF));
	}

	/** Key on (0x214) or off (0x215) by bit mask. */
	void k054539Key (Stream& stream, std::uint32_t time, std::uint16_t offset, std::uint8_t mask)
	{
		k054539Write (stream, time, offset, mask);
	}

	/** `bytes` of a two-level 8-bit waveform at `offset`, one cycle every `period` bytes. */
	void fillK054539Square (std::vector<std::uint8_t>& rom, std::size_t offset, int period,
							std::size_t bytes)
	{
		for (std::size_t i = 0; i < bytes; ++i)
		{
			const bool high = (i % (std::size_t) period) < (std::size_t) period / 2;
			rom[offset + i] = high ? 0x50 : 0xB0;
		}
	}

	/** 440 Hz needs 56320 units a second over a 128-byte cycle; at 18.432 MHz the pitch for that
		is 76896 (one unit per output sample is 0x10000). */
	constexpr std::uint32_t kK054539PitchA4 = 76896;

	/** A non-looping voice ends where the sample's marker byte sits, not at a length register: the
		core keys off when it fetches 0x80. 2048 bytes at 56320 a second is 36 ms. */
	void testK054539Note (void)
	{
		std::vector<std::uint8_t> rom (0x4000, 0);
		fillK054539Square (rom, 0x1000, 128, 2048);
		rom[0x1000 + 2048] = 0x80;		// the end marker

		Stream stream;
		setK054539Voice (stream, 0, 0x1000, k054539Type8, kK054539PitchA4, 0x00);
		k054539Key (stream, 10, 0x214, 0x01);

		const auto notes = runK054539 (stream, 20000, 18432000, rom);

		check (notes.size() == 1, "a K054539 key-on is one note");
		if (notes.empty())
		{
			return;
		}

		check (absoluteNote (notes[0]) == 69, "a 128-byte cycle at 56320 units a second is A4");
		check (notes[0].logicalChannel == 0, "K054539 voice 0 is its own track");
		check (! notes[0].isDrum, "a sample with a period is melodic");
		check (notes[0].velocity == 127, "a volume register of 0 is full scale");
		check (distance (notes[0].endSample, 1614) <= 64,
			   "2048 bytes at 56320 a second is 36 ms, ended by the marker alone");
	}

	/** Half the sample, half the note - and the marker has to be in the ROM for that to work. */
	void testK054539MarkerSetsTheLength (void)
	{
		std::vector<std::uint8_t> rom (0x4000, 0);
		fillK054539Square (rom, 0x1000, 128, 2048);
		rom[0x1000 + 512] = 0x80;

		Stream stream;
		setK054539Voice (stream, 0, 0x1000, k054539Type8, kK054539PitchA4, 0x00);
		k054539Key (stream, 10, 0x214, 0x01);

		const auto notes = runK054539 (stream, 20000, 18432000, rom);

		check (notes.size() == 1 && distance (notes[0].endSample, 411) <= 64,
			   "512 bytes at 56320 a second is 9 ms");
	}

	/** The volume register attenuates: 0x40 is -36 dB, which the extractor hears as silence, and
		0x20 is quiet rather than loud. A decoder that read it as a level would invert both. */
	void testK054539VolumeAttenuates (void)
	{
		std::vector<std::uint8_t> rom (0x4000, 0);
		fillK054539Square (rom, 0x1000, 128, 2048);
		rom[0x1000 + 2048] = 0x80;

		Stream quiet;
		setK054539Voice (quiet, 0, 0x1000, k054539Type8, kK054539PitchA4, 0x20);
		k054539Key (quiet, 10, 0x214, 0x01);

		const auto quietNotes = runK054539 (quiet, 20000, 18432000, rom);
		check (quietNotes.size() == 1 && quietNotes[0].velocity == 17,
			   "-18 dB is a quiet note, not a loud one");

		Stream silent;
		setK054539Voice (silent, 0, 0x1000, k054539Type8, kK054539PitchA4, 0x40);
		k054539Key (silent, 10, 0x214, 0x01);

		check (runK054539 (silent, 20000, 18432000, rom).empty(),
			   "-36 dB is silence for a note");
	}

	/** With the loop bit the marker jumps to the loop address instead of stopping, so only the
		key-off register (0x215) ends the note. */
	void testK054539LoopRunsUntilKeyOff (void)
	{
		std::vector<std::uint8_t> rom (0x4000, 0);
		fillK054539Square (rom, 0x1000, 128, 2048);
		rom[0x1000 + 2048] = 0x80;

		Stream stream;
		setK054539Voice (stream, 0, 0x1000, k054539Type8, kK054539PitchA4, 0x00);
		k054539Write (stream, 0, 0x201, 0x01);		// loop voice 0
		k054539Key (stream, 10, 0x214, 0x01);
		k054539Key (stream, 8000, 0x215, 0x01);

		const auto notes = runK054539 (stream, 20000, 18432000, rom);

		check (notes.size() == 1 && distance (notes[0].endSample, 8000) <= 64,
			   "a looping voice runs past its marker and only a key-off stops it");
	}

	/** The control register's PCM bit gates the chip, and a pitch of zero never steps. */
	void testK054539SilentVoiceIsNoNote (void)
	{
		std::vector<std::uint8_t> rom (0x4000, 0);
		fillK054539Square (rom, 0x1000, 128, 2048);
		rom[0x1000 + 2048] = 0x80;

		Stream noOutput;
		setK054539Voice (noOutput, 0, 0x1000, k054539Type8, kK054539PitchA4, 0x00);
		k054539Write (noOutput, 0, 0x22F, 0x00);		// PCM output off
		k054539Key (noOutput, 10, 0x214, 0x01);
		check (runK054539 (noOutput, 20000, 18432000, rom).empty(),
			   "the control register's PCM bit gates the chip, as the core does");

		Stream noPitch;
		setK054539Voice (noPitch, 0, 0x1000, k054539Type8, 0, 0x00);
		k054539Key (noPitch, 10, 0x214, 0x01);
		check (runK054539 (noPitch, 20000, 18432000, rom).empty(),
			   "a pitch of 0 never fetches a unit");
	}

	/** 16-bit PCM is little-endian pairs, and its marker is the word 0x8000 - a sample that stops
		on a byte 0x80 inside a word would end in the wrong place. */
	void testK054539SixteenBitPcm (void)
	{
		std::vector<std::uint8_t> rom (0x4000, 0);
		for (int i = 0; i < 64 * 8; ++i)		// 8 cycles of 64 16-bit samples
		{
			const bool high = (i % 64) < 32;
			const std::uint16_t value = high ? 0x5000 : 0xB000;
			rom[0x1000 + (std::size_t) i * 2] = (std::uint8_t) (value & 0xFF);
			rom[0x1000 + (std::size_t) i * 2 + 1] = (std::uint8_t) (value >> 8);
		}
		rom[0x1000 + 512 * 2] = 0x00;				// the marker 0x8000
		rom[0x1000 + 512 * 2 + 1] = 0x80;

		Stream stream;
		// 440 Hz over 64 words is 28160 units a second: pitch 38448
		setK054539Voice (stream, 0, 0x1000, k054539Type16, 38448, 0x00);
		k054539Key (stream, 10, 0x214, 0x01);

		const auto notes = runK054539 (stream, 20000, 18432000, rom);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 69,
			   "a 64-word cycle at 28160 words a second is A4");
	}

	/** 4-bit DPCM: the nibbles are deltas, so the cycle only appears after the running sum - and
		the byte 0x88 is the marker, whatever its nibbles would mean. */
	void testK054539FourBitDpcm (void)
	{
		std::vector<std::uint8_t> rom (0x4000, 0);
		std::uint8_t triangle[16];
		for (int i = 0; i < 8; ++i)
		{
			triangle[i] = 0x11;			// sixteen nibbles of +1
			triangle[i + 8] = 0xFF;		// then sixteen of -1: one 32-nibble cycle
		}

		for (std::size_t i = 0; i < 128; ++i)
		{
			rom[0x1000 + i] = triangle[i % 16];		// four cycles
		}
		rom[0x1000 + 128] = 0x88;

		Stream stream;
		// 440 Hz over 32 nibbles is 14080 nibbles a second: pitch 19224
		setK054539Voice (stream, 0, 0x1000, k054539Type4, 19224, 0x00);
		k054539Key (stream, 10, 0x214, 0x01);

		const auto notes = runK054539 (stream, 20000, 18432000, rom);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 69,
			   "a decoded 32-nibble DPCM cycle at 14080 a second is A4");
	}

	/** A drum's attack is smooth: the correlation stays high at every short lag, and the shortest
		lag then wins by being smooth rather than by being a period. These are the first bytes of
		the kick drum in test_file/K054539 (X-Men, "Please.. Rescue The Professor"): the measurement
		used to call it "eight samples", i.e. 2.6 kHz at the pitch the driver uses, and every hit
		became a melodic note an octave below C8. `estimateCycleLength()` now requires the
		correlation to fall away before the peak it picks, which is what makes this a trigger. */
	void testK054539SmoothAttackIsADrum (void)
	{
		const std::uint8_t drum[96] = {
			0x06, 0x0A, 0x0A, 0x0D, 0x10, 0x13, 0x15, 0x18, 0x1C, 0x21, 0x26, 0x2C,
			0x32, 0x38, 0x3E, 0x43, 0x4A, 0x52, 0x59, 0x60, 0x66, 0x6B, 0x6D, 0x6F,
			0x6E, 0x6F, 0x73, 0x76, 0x78, 0x77, 0x76, 0x71, 0x6F, 0x6D, 0x67, 0x5F,
			0x5C, 0x5D, 0x5F, 0x58, 0x4F, 0x4A, 0x4D, 0x51, 0x4E, 0x48, 0x41, 0x40,
			0x43, 0x45, 0x3F, 0x36, 0x2E, 0x2A, 0x29, 0x26, 0x21, 0x1D, 0x1E, 0x1D,
			0x18, 0x0E, 0x06, 0x01, 0x00, 0xFD, 0xF7, 0xEF, 0xE6, 0xDE, 0xDA, 0xDA,
			0xD9, 0xD8, 0xD7, 0xD4, 0xCE, 0xC9, 0xC9, 0xC9, 0xC9, 0xC8, 0xBE, 0xB5,
			0xBA, 0xC0, 0xBE, 0xBB, 0xB8, 0xBA, 0xBD, 0xBD, 0xB8, 0xB3, 0xB3, 0xB4
		};

		std::vector<std::uint8_t> rom (0x4000, 0);
		for (std::size_t i = 0; i < sizeof (drum); ++i)
		{
			rom[0x1000 + i] = drum[i];
		}
		rom[0x1000 + sizeof (drum)] = 0x80;

		Stream stream;
		// the pitch the X-Men driver uses: 21096 units a second
		setK054539Voice (stream, 0, 0x1000, k054539Type8, 28803, 0x00);
		k054539Key (stream, 10, 0x214, 0x01);

		const auto notes = runK054539 (stream, 20000, 18432000, rom);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 60 && notes[0].isDrum,
			   "a smooth attack is a drum hit, not the 2.6 kHz its smoothness suggests");
	}

	/** Retriggering a voice at the same pitch is a new note: the gate never dropped and the pitch
		never moved, so only the key-on counter can say so. Without it a drum or bass line that
		repeats a note collapses into one long note - the X-Men stage theme in test_file/K054539
		came out as a single note per track over 82 seconds. */
	void testK054539RetriggerIsANewNote (void)
	{
		std::vector<std::uint8_t> rom (0x4000, 0);
		fillK054539Square (rom, 0x1000, 128, 2048);
		rom[0x1000 + 2048] = 0x80;

		Stream stream;
		setK054539Voice (stream, 0, 0x1000, k054539Type8, kK054539PitchA4, 0x00);
		k054539Key (stream, 10, 0x214, 0x01);
		k054539Key (stream, 3000, 0x214, 0x01);		// same voice, same pitch, no key-off
		k054539Key (stream, 6000, 0x215, 0x01);

		const auto notes = runK054539 (stream, 20000, 18432000, rom);

		check (notes.size() == 2, "a retrigger at the same pitch is a second note");
		if (notes.size() == 2)
		{
			check (distance (notes[1].startSample, 3000) <= 64,
				   "the second note starts at the second key-on");
		}
	}

	/** Eight voices: the last one's registers start at 0xE0 and its key bit is bit 7. */
	void testK054539Voices (void)
	{
		toybox::chips::K054539Decoder decoder;
		check (decoder.channelCount() == 8, "the K054539 has eight voices");

		std::vector<std::uint8_t> rom (0x4000, 0);
		fillK054539Square (rom, 0x1000, 128, 2048);
		rom[0x1000 + 2048] = 0x80;

		Stream stream;
		setK054539Voice (stream, 7, 0x1000, k054539Type8, kK054539PitchA4, 0x00);
		k054539Key (stream, 10, 0x214, 0x80);

		const auto notes = runK054539 (stream, 20000, 18432000, rom);

		check (notes.size() == 1 && notes[0].logicalChannel == 7,
			   "offset 0xE0 with key bit 7 is voice 7, at A4");
	}

	// --- QSound: sixteen voices, no key bit, and a loop that is a length -------------------------

	std::vector<NoteEvent> runQSound (const Stream& stream, std::uint32_t totalSamples,
									  std::uint32_t clock = 60000000,
									  const std::vector<std::uint8_t>& rom = {})
	{
		toybox::chips::QSoundDecoder decoder;
		decoder.reset (toybox::chips::ChipConfig { clock, 44100.0 });
		if (! rom.empty())
		{
			decoder.setRom (rom);
		}

		return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
	}

	/** QSound writes are a 16-bit value first, then the offset (`0xC4 vh vl ofs`). */
	void qsoundWrite (Stream& stream, std::uint32_t time, std::uint8_t offset, std::uint16_t value)
	{
		RegisterWrite write;
		write.reg = offset;
		write.value = value;
		write.sampleTime = time;
		stream.writes.push_back (write);
	}

	/** One voice: pan centre, start, end, no loop, full volume - the driver's own order, with the
		rate written last because that is what starts the voice. */
	void setQSoundVoice (Stream& stream, int voice, std::uint32_t start, std::uint32_t end,
						 std::uint16_t rate, std::uint16_t volume)
	{
		const std::uint8_t base = (std::uint8_t) (voice * 8);
		qsoundWrite (stream, 0, (std::uint8_t) (0x80 + voice), 0x0120);		// pan centre
		qsoundWrite (stream, 0, (std::uint8_t) (base + 1), (std::uint16_t) start);
		qsoundWrite (stream, 0, (std::uint8_t) (base + 5), (std::uint16_t) end);
		qsoundWrite (stream, 0, (std::uint8_t) (base + 4), 0x0000);			// no loop
		qsoundWrite (stream, 0, (std::uint8_t) (base + 6), volume);
		qsoundWrite (stream, 0, (std::uint8_t) (base + 2), rate);			// key on
	}

	void qsoundKeyOff (Stream& stream, std::uint32_t time, int voice)
	{
		qsoundWrite (stream, time, (std::uint8_t) (voice * 8 + 2), 0x0000);
	}

	void fillQSoundSquare (std::vector<std::uint8_t>& rom, std::size_t offset, int period,
						   std::size_t bytes)
	{
		for (std::size_t i = 0; i < bytes; ++i)
		{
			const bool high = (i % (std::size_t) period) < (std::size_t) period / 2;
			rom[offset + i] = high ? 0x50 : 0xB0;
		}
	}

	/** At 60 MHz a voice advances `rate * 60e6 / 10223616` bytes a second, so rate 0x1000 plays a
		byte per output sample (24038 a second) and a 55-byte cycle is 437 Hz - A4. The end address
		ends it: 2048 bytes is 85 ms, with no key-off anywhere. */
	void testQSoundNote (void)
	{
		std::vector<std::uint8_t> rom (0x8000, 0);
		fillQSoundSquare (rom, 0x1000, 55, 2048);

		Stream stream;
		setQSoundVoice (stream, 0, 0x1000, 0x1800, 0x1000, 0x4000);

		const auto notes = runQSound (stream, 20000, 60000000, rom);

		check (notes.size() == 1, "a QSound voice that starts sounding is one note");
		if (notes.empty())
		{
			return;
		}

		check (absoluteNote (notes[0]) == 69, "a 55-byte cycle at 24038 bytes a second is A4");
		check (notes[0].logicalChannel == 0, "QSound voice 0 is its own track");
		check (! notes[0].isDrum, "a sample with a period is melodic");
		check (notes[0].velocity == 127, "a full-scale volume register is full velocity");
		check (distance (notes[0].endSample, 3757) <= 80,
			   "the end address ends the note: 2048 bytes at 24038 a second");
	}

	/** There is no key bit: the rate going from zero to non-zero is the key-on, and back to zero is
		the key-off - which is exactly what the core's mixer plays. */
	void testQSoundRateIsTheKey (void)
	{
		std::vector<std::uint8_t> rom (0x8000, 0);
		fillQSoundSquare (rom, 0x1000, 55, 4096);

		Stream stream;
		setQSoundVoice (stream, 0, 0x1000, 0x2000, 0x1000, 0x4000);
		qsoundKeyOff (stream, 5000, 0);

		const auto notes = runQSound (stream, 20000, 60000000, rom);

		check (notes.size() == 1 && distance (notes[0].endSample, 5000) <= 64,
			   "writing the rate back to zero ends the note");
	}

	/** The loop is a length: the voice wraps from `end` back `loop_len` bytes, so a sustained note
		repeats that region and only the rate going to zero stops it. */
	void testQSoundLoopIsALength (void)
	{
		std::vector<std::uint8_t> rom (0x8000, 0);
		fillQSoundSquare (rom, 0x1000, 55, 4096);

		Stream stream;
		qsoundWrite (stream, 0, 0x80, 0x0120);
		qsoundWrite (stream, 0, 0x01, 0x1000);		// start
		qsoundWrite (stream, 0, 0x05, 0x1800);		// end
		qsoundWrite (stream, 0, 0x04, 0x00DC);		// loop 220 bytes = four 55-byte cycles
		qsoundWrite (stream, 0, 0x06, 0x4000);
		qsoundWrite (stream, 0, 0x02, 0x1000);

		qsoundKeyOff (stream, 6000, 0);

		const auto notes = runQSound (stream, 20000, 60000000, rom);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 69,
			   "a looping QSound voice keeps the note its loop measures");
		check (notes.size() == 1 && distance (notes[0].endSample, 6000) <= 64,
			   "and only the rate going to zero ends it");
	}

	/** A rate written while a voice is already sounding is a slide, not a key-on. The QSound Logo
		(`test_file/QSound/01 QSound Logo.vgz`) writes the rate nine times inside one sustained note,
		and keying on every write turned the melody into a burst of triggers. */
	void testQSoundRateSlideIsNotANewNote (void)
	{
		std::vector<std::uint8_t> rom (0x8000, 0);
		fillQSoundSquare (rom, 0x1000, 55, 4096);

		Stream stream;
		qsoundWrite (stream, 0, 0x80, 0x0120);
		qsoundWrite (stream, 0, 0x01, 0x1000);
		qsoundWrite (stream, 0, 0x05, 0x1800);
		qsoundWrite (stream, 0, 0x04, 0x00DC);		// loops, so only a zero rate ends it
		qsoundWrite (stream, 0, 0x06, 0x4000);
		qsoundWrite (stream, 0, 0x02, 0x1000);		// key on
		qsoundWrite (stream, 1000, 0x02, 0x1040);	// vibrato: a third of a semitone, still A4
		qsoundWrite (stream, 2000, 0x02, 0x1080);
		qsoundKeyOff (stream, 6000, 0);

		const auto notes = runQSound (stream, 20000, 60000000, rom);

		// The rate writes are a slide, not key-ons: the note starts at the key-on and any further
		// entry is the *same* note split where the vibrato crosses a semitone boundary. Which
		// boundary that is moved when the pitch became L1 (a constant offset), which is why this
		// counts starts rather than notes - the property under test is that no key-on happens.
		check (! notes.empty() && notes.front().startSample == 0,
			   "a rate written while sounding slides the note, it does not start one");
		check (notes.size() <= 2, "the vibrato can only split the note once, at a semitone boundary");
	}

	/** The chip addresses eight megabytes - a 15-bit bank of 64 KB pages - and the QSound Logo's
		samples sit at 0x520000 and 0x640000. A ROM image clipped to four megabytes reads zeroes
		there, and every voice then measures no period and becomes a C4 trigger. */
	void testQSoundRomBeyondFourMegabytes (void)
	{
		std::vector<std::uint8_t> rom (5 * 1024 * 1024, 0);
		fillQSoundSquare (rom, 0x480000, 55, 2048);

		Stream stream;
		qsoundWrite (stream, 0, 0x00, 0x8048);		// voice 0's bank register -> voice 1, bank 0x48
		setQSoundVoice (stream, 1, 0x0000, 0x0800, 0x1000, 0x4000);

		const auto notes = runQSound (stream, 20000, 60000000, rom);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 69,
			   "a sample at 0x480000 is reachable: the QSound addresses eight megabytes, not four");
	}

	/** A drum has no cycle to measure: noise stays the C4 trigger, exactly as on the K053260 and
		the K054539, because the Street Fighter II drum kit shares these sixteen voices. */
	void testQSoundPercussion (void)
	{
		std::vector<std::uint8_t> rom (0x8000, 0);
		std::uint32_t seed = 987654321u;
		for (std::size_t i = 0; i < 2048; ++i)
		{
			seed = seed * 1103515245u + 12345u;
			rom[0x2000 + i] = (std::uint8_t) (seed >> 16);
		}

		Stream stream;
		setQSoundVoice (stream, 1, 0x2000, 0x2800, 0x1000, 0x4000);

		const auto notes = runQSound (stream, 20000, 60000000, rom);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 60 && notes[0].isDrum,
			   "a sample with no period is the C4 drum trigger");
		check (notes.size() == 1 && notes[0].channelKind == toybox::chips::ChannelKind::adpcmDrum,
			   "and its family says the pitch is not real");
	}

	/** A bank register belongs to the next voice (init_register_map in the core): writing offset 0
		of voice 0 points voice 1 at another 64 KB bank. Voice 0's own sample is noise, so a decoder
		that gave the bank to the wrong voice makes voice 1 a drum instead of the A4 in bank 1. */
	void testQSoundBankBelongsToTheNextVoice (void)
	{
		std::vector<std::uint8_t> rom (0x20000, 0);
		std::uint32_t seed = 12345u;
		for (std::size_t i = 0; i < 2048; ++i)			// bank 0: noise
		{
			seed = seed * 1103515245u + 12345u;
			rom[0x1000 + i] = (std::uint8_t) (seed >> 16);
		}
		fillQSoundSquare (rom, 0x11000, 55, 2048);		// bank 1: a periodic waveform

		Stream stream;
		qsoundWrite (stream, 0, 0x00, 0x8001);			// voice 0's bank register -> voice 1
		setQSoundVoice (stream, 1, 0x1000, 0x1800, 0x1000, 0x4000);

		const auto notes = runQSound (stream, 20000, 60000000, rom);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 69,
			   "the bank register of voice 0 addresses voice 1, which is why this is A4");
	}

	/** The volume is the only silence: a pan of 0 still sounds (it is hard left), but a zero
		volume register is not a note. */
	void testQSoundSilentVoiceIsNoNote (void)
	{
		std::vector<std::uint8_t> rom (0x8000, 0);
		fillQSoundSquare (rom, 0x1000, 55, 2048);

		Stream quiet;
		qsoundWrite (quiet, 0, 0x80, 0x0000);			// pan 0: hard left, still audible
		qsoundWrite (quiet, 0, 0x01, 0x1000);
		qsoundWrite (quiet, 0, 0x05, 0x1800);
		qsoundWrite (quiet, 0, 0x06, 0x0000);			// volume 0
		qsoundWrite (quiet, 0, 0x02, 0x1000);

		check (runQSound (quiet, 20000, 60000000, rom).empty(),
			   "a voice at zero volume is not a note");
	}

	/** Nineteen channels: sixteen PCM and three ADPCM, and the ADPCM voices start from their own
		flag register (`0xD6+n`) rather than from a rate. */
	void testQSoundChannelsAndAdpcm (void)
	{
		toybox::chips::QSoundDecoder decoder;
		check (decoder.channelCount() == 19, "QSound has sixteen PCM voices and three ADPCM ones");

		std::vector<std::uint8_t> rom (0x8000, 0);
		fillQSoundSquare (rom, 0x3000, 55, 2048);

		Stream stream;
		qsoundWrite (stream, 0, 0x90, 0x0120);			// ADPCM 1 pan centre
		qsoundWrite (stream, 0, 0xCA, 0x3000);			// start
		qsoundWrite (stream, 0, 0xCB, 0x3800);			// end
		qsoundWrite (stream, 0, 0xCC, 0x8000);			// bank 0
		qsoundWrite (stream, 0, 0xCD, 0x4000);			// volume
		qsoundWrite (stream, 0, 0xD6, 0x0001);			// start playback

		const auto notes = runQSound (stream, 30000, 60000000, rom);

		check (notes.size() == 1 && notes[0].logicalChannel == 16,
			   "an ADPCM flag write starts voice 17 of 19");
		check (notes.size() == 1 && distance (notes[0].endSample, 22540) <= 80,
			   "its end address ends it: 2048 bytes is 4096 nibbles at 8 kHz, half a second");
	}

	// --- GA20: four voices, a 16-byte address unit and a sample that ends on 0x00 ------------------

	std::vector<NoteEvent> runGa20 (const Stream& stream, std::uint32_t totalSamples,
								   const std::vector<std::uint8_t>& rom)
	{
		toybox::chips::Ga20Decoder decoder;
		decoder.reset (toybox::chips::ChipConfig { 3579545, 44100.0 });
		decoder.setRom (rom);
		return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
	}

	/** `0xBF <register> <value>`, register = channel * 8 + n. */
	void ga20Write (Stream& stream, std::uint32_t time, std::uint8_t reg, std::uint8_t value)
	{
		RegisterWrite write;
		write.reg = reg;
		write.value = value;
		write.sampleTime = time;
		stream.writes.push_back (write);
	}

	/** Start, rate, volume - and the control bit that keys the voice on. */
	void startGa20Voice (Stream& stream, std::uint32_t time, int channel, std::uint32_t start,
						 std::uint8_t rate, std::uint8_t volume)
	{
		const std::uint8_t base = (std::uint8_t) (channel * 8);
		ga20Write (stream, time, (std::uint8_t) (base + 0), (std::uint8_t) ((start >> 4) & 0xFF));
		ga20Write (stream, time, (std::uint8_t) (base + 1), (std::uint8_t) ((start >> 12) & 0xFF));
		ga20Write (stream, time, (std::uint8_t) (base + 4), rate);
		ga20Write (stream, time, (std::uint8_t) (base + 5), volume);
		ga20Write (stream, time, (std::uint8_t) (base + 6), 0x02);
	}

	/** A periodic sample of `length` bytes, then the ROM's zeroes - which are the sample's end
		marker, the only thing that stops a GA20 note early. */
	std::vector<std::uint8_t> ga20Square (std::size_t size, std::size_t offset, int cycle,
										  std::size_t length = 512)
	{
		std::vector<std::uint8_t> rom (size, 0);
		for (std::size_t i = 0; i < length && offset + i < size; ++i)
		{
			rom[offset + i] = ((i % (std::size_t) cycle) < (std::size_t) cycle / 2) ? 0x50 : 0xB0;
		}
		return rom;
	}

	/** An 18-byte cycle at rate 131: the core runs at clock/4 = 894,886 samples a second and one
		byte lasts 256-131 = 125 of them, so the sample advances 7159 bytes a second and 7159/18 =
		397.7 Hz is G4. A decoder that used the output rate (44,100/125 = 353 bytes a second) would
		read 19.6 Hz here, which is not a note at all. The sample's 512 bytes then hit the 0x00
		terminator, the only end the core reads (there is no loop). */
	void testGa20Note (void)
	{
		Stream stream;
		startGa20Voice (stream, 0, 0, 0x4000, 131, 0xFF);

		const auto notes = runGa20 (stream, 20000, ga20Square (0x10000, 0x4000, 18));

		check (notes.size() == 1, "a GA20 key-on with a periodic sample is one note");
		if (notes.empty())
		{
			return;
		}

		check (absoluteNote (notes[0]) == 67, "an 18-byte cycle at 7159 bytes a second is G4");
		check (notes[0].logicalChannel == 0, "GA20 voice 0 is its own track");
		check (! notes[0].isDrum, "a sample with a period is melodic");
		check (notes[0].channelKind == toybox::chips::ChannelKind::ga20Pcm, "and its family says so");
		check (notes[0].velocity == 127, "full-scale volume is full velocity");
		check (distance (notes[0].endSample, 3153) <= 80,
			   "the 0x00 terminator ends the note: 512 bytes at 125 samples of clock/4 each");
	}

	/** The control bit is the gate: writing 0 ends the note where the driver says, not where the
		sample would have ended. */
	void testGa20KeyOff (void)
	{
		Stream stream;
		startGa20Voice (stream, 0, 0, 0x4000, 131, 0xFF);
		ga20Write (stream, 1000, 0x06, 0x00);

		const auto notes = runGa20 (stream, 20000, ga20Square (0x10000, 0x4000, 18));

		check (notes.size() == 1 && distance (notes[0].endSample, 1000) <= 64,
			   "writing the control bit back to zero ends the note");
	}

	/** A drum has no period to measure, so it stays the C4 trigger - these boards put drums and
		melody on the same four voices, and the decision is per key-on. */
	void testGa20Percussion (void)
	{
		std::vector<std::uint8_t> rom (0x10000, 0);
		std::uint32_t seed = 24681357u;
		for (std::size_t i = 0; i < 512; ++i)
		{
			seed = seed * 1103515245u + 12345u;
			rom[0x5000 + i] = (std::uint8_t) (seed >> 16);
		}

		Stream stream;
		startGa20Voice (stream, 0, 1, 0x5000, 131, 0xFF);

		const auto notes = runGa20 (stream, 20000, rom);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 60 && notes[0].isDrum,
			   "a GA20 sample with no period is the C4 drum trigger");
	}

	/** The start registers are in 16-byte units: register 0x0400 addresses byte 0x4000, so a decoder
		that treats them as a byte address reads zeroes and plays nothing. */
	void testGa20AddressUnit (void)
	{
		Stream wrong;
		startGa20Voice (wrong, 0, 0, 0x0040, 131, 0xFF);		// 0x40 is silence in this ROM

		check (runGa20 (wrong, 20000, ga20Square (0x10000, 0x4000, 18)).empty(),
			   "start 0x0040 reads the empty part of the ROM, so the address is not a byte address");

		Stream right;
		startGa20Voice (right, 0, 0, 0x4000, 131, 0xFF);

		const auto notes = runGa20 (right, 20000, ga20Square (0x10000, 0x4000, 18));

		check (notes.size() == 1 && absoluteNote (notes[0]) == 67,
			   "start 0x4000 is register 0x0400: sixteen bytes a step");
	}

	// --- OKIM6295: a two-byte command port, no pitch register, the sample's cycle is the note ------

	std::vector<NoteEvent> runOkim6295 (const Stream& stream,
										const std::vector<std::uint8_t>& rom,
										std::uint32_t totalSamples = 44100)
	{
		toybox::chips::Okim6295Decoder decoder;
		decoder.reset (toybox::chips::ChipConfig { 1000000, 44100.0 });
		decoder.setRom (rom);
		return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
	}

	/** A 4 KB ROM whose sample 0 runs 0x0200..0x02FF, with `byte` in every position: the sample
		table is three bytes of start and three of stop, big endian, at `sample * 8`. */
	std::vector<std::uint8_t> okimRom (std::uint8_t byte)
	{
		std::vector<std::uint8_t> rom (4096, byte);
		rom[0] = 0x00;			// start 0x000200
		rom[1] = 0x02;
		rom[2] = 0x00;
		rom[3] = 0x00;			// stop 0x0002FF
		rom[4] = 0x02;
		rom[5] = 0xFF;
		return rom;
	}

	/** The register interface is a command port: bit 7 of the first byte names the sample, the
		second byte's bits 4-7 start voices and its low nibble is the volume (0 is the loudest).
		The pitch is the sample's own cycle - nibbles of +2 then -2 walk a period of 16 - because
		the chip has no pitch register at all. */
	void testOkim6295Notes (void)
	{
		std::vector<std::uint8_t> rom = okimRom (0x00);
		for (std::size_t i = 0x200; i <= 0x2FF; ++i)
		{
			rom[i] = ((i - 0x200) % 16 < 8) ? 0x02 : 0x0E;		// eight up, eight down: 16 nibbles
		}

		Stream stream;
		stream.at (0, 0, 0x80);			// sample 0
		stream.at (0, 0, 0x10);			// voice 0 starts, volume 0 (the loudest)
		const auto notes = runOkim6295 (stream, rom);

		check (notes.size() == 1, "an OKIM6295 start is one note");
		if (notes.empty())
		{
			return;
		}

		check (notes[0].logicalChannel == 0, "the mask's bit 4 is voice 0");
		check (! notes[0].isDrum, "a periodic sample is melodic even without a pitch register");
		check (notes[0].velocity == 127, "volume 0 is full scale");
		// the exact octave depends on how many nibbles of this triangle the correlator locks onto,
		// which is the nature of a chip with no pitch register - what has to hold is that the note
		// comes from the sample (not the 60 marker) and says so
		check (notes[0].note != 60 && notes[0].pitchSource == toybox::chips::PitchSource::sampleInferred,
			   "the note comes from the sample's measured cycle");
	}

	/** Bit 7 of the start byte is part of the voice mask when a sample is pending, which is the
		only way voice 3 can start: reading it as "sample select" left that voice silent. */
	void testOkim6295FourthVoice (void)
	{
		Stream stream;
		stream.at (0, 0, 0x80);
		stream.at (0, 0, 0x8F);			// bit 7 set *and* mask bit 7: voice 3, volume 15

		const auto notes = runOkim6295 (stream, okimRom (0x00));

		check (notes.size() == 1 && notes[0].logicalChannel == 3,
			   "voice 3 starts through a byte that also has bit 7 set");
		check (notes.size() == 1 && notes[0].velocity < 40, "volume 15 is the quietest step");
	}

	/** A sample with no period - the constant output of a zeroed ROM - is the C4 trigger. */
	void testOkim6295Unpitched (void)
	{
		Stream stream;
		stream.at (0, 0, 0x80);
		stream.at (0, 0, 0x10);

		const auto notes = runOkim6295 (stream, okimRom (0x00));

		check (notes.size() == 1 && notes[0].isDrum && notes[0].note == 60,
			   "an OKIM6295 sample with no period is the C4 drum trigger");
	}

	/** K005289: two wavetable voices whose pitch is arithmetic, not measured - `clock / (32 *
		(freq + 1))` - the divider arriving inverted and the TG write acting as the key. */

	void testK005289Notes (void)
	{
		std::vector<toybox::chips::RegisterWrite> writes;
		const auto add = [&writes] (std::uint8_t reg, std::uint16_t data, std::uint32_t time)
		{
			toybox::chips::RegisterWrite write;
			write.reg = reg;
			write.value = data;
			write.sampleTime = time;
			writes.push_back (write);
		};

		add (0x00, 0x000F, 0);				// volume 15, waveform 0
		add (0x02, 0x0FFF - 249, 0);		// LD: the driver's inverted divider, freq becomes 249
		add (0x04, 0x0000, 0);				// TG: the trigger

		toybox::chips::K005289Decoder decoder;
		decoder.reset (toybox::chips::ChipConfig { 4000000, 44100.0 });

		const auto performance = toybox::chips::extractPerformance (decoder, writes, 44100);
		const auto& notes = performance.notes;

		check (notes.size() == 1, "a K005289 TG write is one note");
		if (notes.empty())
		{
			return;
		}

		check (notes[0].logicalChannel == 0, "address bit 0 selects the voice");
		check (! notes[0].isDrum, "a wavetable voice is melodic");
		// 4 MHz / (32 * 250) is 500 Hz, and 500 Hz is B4 (71) plus 21 cents
		check (notes[0].note == 71, "the divider is arithmetic: 500 Hz is B4");
	}
	/** VSU: five wavetable voices whose pitch is `clock / (32 * (2048 - freq))`, keyed by bit 7 of
		the voice's IntlControl byte, and a sixth voice that is noise. */

	/** MSM5232: the register is not a divider but a key index into the chip's eighty-eight-entry
		ROM, and eighty-eight entries is the span from A0, so the index is the note: 21 + index. */

	/** FTM is the first format whose notes are not inferred.

		The module states note, octave and row, so the stream comes out authored: no decoder runs and
		the encoder sees the same `NoteStream` it has always seen. The synthetic module below pins
		three things the real files cannot state on their own - the container framing, the row layout,
		and the time model (tempo 150 with speed 6 is ten rows a second). */


	/** Portamento (1xx) slides a note's pitch away from it. The magnitude is an approximation -
		FamiTracker counts in period units, this counts 1/16 semitone - so the test pins the shape and
		the direction, and the code says which part is approximate. */


	/** The order list is walked, not played straight through. A `Dxx` on the first frame must skip
		the second one, so its note never appears - which the real files could only hint at. */


	/** H-3: the two shapes the sampled decoders kept writing by hand. The point of the helpers is
		that "not melodic" and "silence" are different answers - a drum is a C4 trigger, a stopped
		voice is no note at all - so both are pinned here. */

	void testSampledNoteHelpers (void)
	{
		int fine = 0;

		// 1000 bytes a second over a two-byte cycle is 500 Hz: B4
		check (toybox::chips::noteFromSampleCycle (1000.0, 2.0, true, 60, &fine) == 71,
			   "a measured cycle is the whole pitch");

		check (toybox::chips::noteFromSampleCycle (1000.0, 2.0, false, 60, &fine) == 60,
			   "an unmelodic sample is the one-shot trigger, not a note");

		check (toybox::chips::noteFromSampleCycle (0.0, 2.0, true, 60, &fine) == -1,
			   "a stopped voice is silent, not a drum");

		// the register's own rate against the chip's reference: unity is middle C
		check (toybox::chips::noteFromPlaybackRate (8000.0, 8000.0, true, 60, &fine) == 60,
			   "unity playback rate is middle C");

		check (toybox::chips::noteFromPlaybackRate (16000.0, 8000.0, true, 60, &fine) == 72,
			   "twice the reference rate is an octave up");

		check (toybox::chips::noteFromPlaybackRate (8000.0, 8000.0, false, 60, &fine) == 60,
			   "an unmelodic sample reports the trigger");

		check (toybox::chips::noteFromPlaybackRate (0.0, 8000.0, true, 60, &fine) == -1,
			   "a zero rate stays silent");
	}

	/** M-1: the two pitch camps. A decoder must fill one of them; a channel that fills neither is
		silent in MIDI with nothing to show for it, which is the failure this contract exists to catch. */

	void testPitchFieldsContract (void)
	{
		using toybox::chips::ChannelState;
		using toybox::chips::PitchFields;
		using toybox::chips::PitchSource;

		const ChannelState fresh;
		check (toybox::chips::pitchFieldsOf (fresh) == PitchFields::none,
			   "a fresh channel says nothing about pitch");

		ChannelState period;
		period.present = true;
		period.gate = true;
		period.period = 0x2A4;
		period.block = 4;
		period.pitchSource = PitchSource::registerExact;
		check (toybox::chips::pitchFieldsOf (period) == PitchFields::period,
			   "an FM state is the period camp");
		check (toybox::chips::isPitchStateSane (period), "the period camp passes the check");

		ChannelState frequency;
		frequency.present = true;
		frequency.gate = true;
		frequency.hz = 440.0;
		frequency.pitchSource = PitchSource::registerExact;
		check (toybox::chips::pitchFieldsOf (frequency) == PitchFields::frequency,
			   "a rate state is the frequency camp");
		check (toybox::chips::isPitchStateSane (frequency), "the frequency camp passes the check");

		ChannelState broken;
		broken.present = true;
		broken.gate = true;
		broken.pitchSource = PitchSource::registerExact;
		check (! toybox::chips::isPitchStateSane (broken),
			   "a voiced channel with neither camp is rejected");

		ChannelState drum;
		drum.present = true;
		drum.gate = true;
		drum.pitchSource = PitchSource::unpitched;
		check (toybox::chips::isPitchStateSane (drum), "a drum is exempt, it reports a C4 trigger");
	}

	/** The VGM loader is the seam between format and assembly, so it is tested on its own: a
		synthetic stream must come out as chip kinds and instances, a sixteen-bit value already packed,
		and the data block as a view rather than a copy. */


	/** MIKEY: four timer-driven squares at `clock / (2 * (backup + 1) * 2^(shift + 4))`. */

	void testMikeyNotes (void)
	{
		std::vector<toybox::chips::RegisterWrite> writes;
		const auto add = [&writes] (std::uint8_t reg, std::uint8_t value)
		{
			toybox::chips::RegisterWrite write;
			write.reg = reg;
			write.value = value;
			write.sampleTime = 0;
			writes.push_back (write);
		};

		add (0x20, 0x0F);		// channel 0 volume
		add (0x24, 124);		// BACKUP: the reload value
		add (0x25, 0x03);		// CONTROL A: clock shift 3

		toybox::chips::MikeyDecoder decoder;
		decoder.reset (toybox::chips::ChipConfig { 16000000, 44100.0 });

		const auto performance = toybox::chips::extractPerformance (decoder, writes, 44100);
		const auto& notes = performance.notes;

		check (notes.size() == 1, "a Mikey backup write is one note");
		if (notes.empty())
		{
			return;
		}

		check (notes[0].logicalChannel == 0, "0x20 is the first audio channel");
		// 16 MHz / (2 * 125 * 128) is 500 Hz, and 500 Hz is B4
		check (notes[0].note == 71, "the timer arithmetic puts 500 Hz on B4");
	}
	void testMsm5232Notes (void)
	{
		std::vector<toybox::chips::RegisterWrite> writes;
		toybox::chips::RegisterWrite write;
		write.reg = 0x00;							// voice 0
		write.value = (std::uint8_t) (0x80 | 48);	// gate, key A4
		write.sampleTime = 0;
		writes.push_back (write);

		toybox::chips::Msm5232Decoder decoder;
		decoder.reset (toybox::chips::ChipConfig { 2119040, 44100.0 });

		const auto performance = toybox::chips::extractPerformance (decoder, writes, 44100);
		const auto& notes = performance.notes;

		check (notes.size() == 1, "an MSM5232 key write is one note");
		if (notes.empty())
		{
			return;
		}

		check (notes[0].logicalChannel == 0, "register 0 is the first voice");
		check (notes[0].note == 69, "index 48 is A4, because 21 + 48 = 69");
	}
	void testVsuNotes (void)
	{
		std::vector<toybox::chips::RegisterWrite> writes;
		const auto add = [&writes] (std::uint8_t reg, std::uint8_t value, std::uint32_t time)
		{
			toybox::chips::RegisterWrite write;
			write.reg = reg;
			write.value = value;
			write.sampleTime = time;
			writes.push_back (write);
		};

		// voice 0: 2048 - 1667 = 381, and 5 370 000 / (32 * 381) is 440 Hz - A4
		add (0x02, 0x83, 0);				// frequency, low byte
		add (0x03, 0x06, 0);				// frequency, high three bits
		add (0x00, 0x80, 0);				// IntlControl: bit 7 keys the voice

		toybox::chips::VsuDecoder decoder;
		decoder.reset (toybox::chips::ChipConfig { 5370000, 44100.0 });

		const auto performance = toybox::chips::extractPerformance (decoder, writes, 44100);
		const auto& notes = performance.notes;

		check (notes.size() == 1, "a VSU key write is one note");
		if (notes.empty())
		{
			return;
		}

		check (notes[0].logicalChannel == 0, "the first voice is channel 0");
		check (notes[0].note == 69, "clock / (32 * (2048 - freq)) puts 381 on A4");
	}

	// --- ES5503: thirty-two wavetable oscillators, a halt bit as the key and a table's own cycle ----

	std::vector<NoteEvent> runEs5503 (const Stream& stream, std::uint32_t totalSamples,
									  const std::vector<std::uint8_t>& ram,
									  std::uint32_t clock = 7159090)
	{
		toybox::chips::Es5503Decoder decoder;
		decoder.reset (toybox::chips::ChipConfig { clock, 44100.0 });
		decoder.setRam (ram);
		return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
	}

	/** `0xD5 <port> <register> <value>` - the register space is the chip's own. */
	void es5503Write (Stream& stream, std::uint32_t time, std::uint8_t reg, std::uint8_t value)
	{
		RegisterWrite write;
		write.reg = reg;
		write.value = value;
		write.sampleTime = time;
		stream.writes.push_back (write);
	}

	/** Pointer, table size and resolution, frequency, full volume - then the control byte that
		starts the oscillator (bit 0 is the halt bit, so clearing it is the key-on). */
	void startEs5503Voice (Stream& stream, std::uint32_t time, int osc, std::uint16_t frequency,
						   std::uint8_t pointerHigh, std::uint8_t sizeAndResolution,
						   std::uint8_t control = 0x00)
	{
		const std::uint8_t base = (std::uint8_t) osc;
		es5503Write (stream, time, 0xE1, 0x3E);			// thirty-two oscillators
		es5503Write (stream, time, (std::uint8_t) (0x80 + base), pointerHigh);
		es5503Write (stream, time, (std::uint8_t) (0xC0 + base), sizeAndResolution);
		es5503Write (stream, time, (std::uint8_t) (0x00 + base), (std::uint8_t) (frequency & 0xFF));
		es5503Write (stream, time, (std::uint8_t) (0x20 + base), (std::uint8_t) (frequency >> 8));
		es5503Write (stream, time, (std::uint8_t) (0x40 + base), 0xFF);
		es5503Write (stream, time, (std::uint8_t) (0xA0 + base), control);
	}

	/** A table of `cycle`-byte waves repeated over `length` bytes. */
	std::vector<std::uint8_t> es5503Ram (std::size_t tableOffset, std::size_t cycle, std::size_t length,
										 std::size_t ramSize = 128 * 1024)
	{
		std::vector<std::uint8_t> ram (ramSize, 0);
		for (std::size_t i = 0; i < length && tableOffset + i < ramSize; ++i)
		{
			ram[tableOffset + i] = ((i % cycle) < cycle / 2) ? 0x50 : 0xB0;
		}
		return ram;
	}

	/** With thirty-two oscillators enabled the chip runs at (7159090/8)/34 = 26,320 samples a
		second, and a 1024-byte table at frequency 8,000 is 8000 * 26320 / 512 / 1024 = 401.6 Hz -
		G4. A decoder that read the table as ten times shorter, or that used the output rate as the
		chip's, lands somewhere else entirely. */
	void testEs5503Note (void)
	{
		Stream stream;
		startEs5503Voice (stream, 0, 0, 8000, 0x00, 0x12);

		const auto notes = runEs5503 (stream, 30000, es5503Ram (0x0000, 1024, 1024));

		check (notes.size() == 1, "an ES5503 key-on with a table is one note");
		if (notes.empty())
		{
			return;
		}

		check (notes[0].logicalChannel == 0, "ES5503 oscillator 0 is its own track");
		check (absoluteNote (notes[0]) == 67, "a 1024-byte table at 8,000 is G4");
		check (! notes[0].isDrum, "a table with a pitch is melodic");
		check (notes[0].channelKind == toybox::chips::ChannelKind::wavetableWave, "and its family says so");
		check (notes[0].velocity == 127, "full-scale volume is full velocity");
	}

	/** The same 1024 bytes holding four cycles of a 256-byte wave is the same note at a quarter of
		the frequency register - the chip repeats the *wave*, not the table. A decoder that divided
		by the table size would report G2 here. */
	void testEs5503TableCycle (void)
	{
		Stream stream;
		startEs5503Voice (stream, 0, 1, 2000, 0x00, 0x12);

		const auto notes = runEs5503 (stream, 30000, es5503Ram (0x0000, 256, 1024));

		check (notes.size() == 1 && absoluteNote (notes[0]) == 67,
			   "four 256-byte cycles in 1024 bytes are the same G4 at a quarter of the frequency");
	}

	/** Control bit 0 is the halt bit: setting it is the key-off. A free-running table has no end
		of its own, so that write is the only end there is. */
	void testEs5503KeyOff (void)
	{
		Stream stream;
		startEs5503Voice (stream, 0, 2, 8000, 0x00, 0x12);
		es5503Write (stream, 1000, (std::uint8_t) (0xA0 + 2), 0x01);

		const auto notes = runEs5503 (stream, 30000, es5503Ram (0x0000, 1024, 1024));

		check (notes.size() == 1 && distance (notes[0].endSample, 1000) <= 64,
			   "setting the halt bit ends a free-running ES5503 note");
	}

	/** A one-shot table ends when it runs out, and that length is exact: 32,768 bytes at 8,000 is
		411,253 bytes a second, so 79.7 ms - 3,515 samples. */
	void testEs5503OneShot (void)
	{
		Stream stream;
		startEs5503Voice (stream, 0, 3, 8000, 0x00, 0x3F, 0x02);		// size 7, one-shot

		const auto notes = runEs5503 (stream, 60000, es5503Ram (0x0000, 1024, 32768));

		check (notes.size() == 1, "a one-shot ES5503 voice is one note");
		if (notes.empty())
		{
			return;
		}

		check (distance (notes[0].endSample, 3515) <= 120,
			   "a one-shot ends at the table's length: 32768 entries at 411,253 a second");
	}

	/** A table with no period that does not loop is a drum - these boards put drums on the same
		thirty-two oscillators as the music. */
	void testEs5503Percussion (void)
	{
		std::vector<std::uint8_t> ram (128 * 1024, 0);
		std::uint32_t seed = 24681357u;
		for (std::size_t i = 0; i < 1024; ++i)
		{
			seed = seed * 1103515245u + 12345u;
			ram[0x0000 + i] = (std::uint8_t) (seed >> 16);
		}

		Stream stream;
		startEs5503Voice (stream, 0, 4, 8000, 0x00, 0x12, 0x02);

		const auto notes = runEs5503 (stream, 30000, ram);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 60 && notes[0].isDrum,
			   "a one-shot ES5503 table with no period is the C4 drum trigger");
	}

	/** The oscillator count is part of the pitch: the core runs at `clock / 8 / (enabled + 2)`, so
		the same registers on a chip with two oscillators enabled walk their tables four times
		faster than on one with thirty-two. */
	void testEs5503OscillatorCount (void)
	{
		Stream many;
		startEs5503Voice (many, 0, 5, 500, 0x00, 0x12);		// 0xE1 = 0x3E, thirty-two

		Stream few;
		es5503Write (few, 0, 0xE1, 0x02);					// (0x02 >> 1) + 1 = two
		es5503Write (few, 0, 0x85, 0x00);
		es5503Write (few, 0, 0xC5, 0x12);
		es5503Write (few, 0, 0x05, (std::uint8_t) (500 & 0xFF));
		es5503Write (few, 0, 0x25, 0x01);
		es5503Write (few, 0, 0x45, 0xFF);
		es5503Write (few, 0, 0xA5, 0x00);

		const auto ram = es5503Ram (0x0000, 1024, 1024);
		const auto slow = runEs5503 (many, 30000, ram);
		const auto fast = runEs5503 (few, 30000, ram);

		check (! slow.empty() && ! fast.empty() && slow[0].note != fast[0].note,
			   "fewer oscillators enabled means a faster chip, so the same registers are a higher note");
		check (! slow.empty() && absoluteNote (slow[0]) == 19, "thirty-two oscillators at 500 is E0");
		check (! fast.empty() && absoluteNote (fast[0]) == 56, "two oscillators at 500 is G#3 (213 Hz)");
	}

	// --- Sega PCM: sixteen voices, registers as playback state, an end address in 256-byte units --

	std::vector<NoteEvent> runSegaPcm (const Stream& stream, std::uint32_t totalSamples,
									   const std::vector<std::uint8_t>& rom,
									   std::uint32_t clock = 4026987)
	{
		toybox::chips::SegaPcmDecoder decoder;
		decoder.reset (toybox::chips::ChipConfig { clock, 44100.0 });
		decoder.setRom (rom);
		return toybox::chips::extractNotes (decoder, stream.writes, totalSamples);
	}

	/** `0xC0 <offset low> <offset high> <value>`, a byte of the chip's register RAM. */
	void segaWrite (Stream& stream, std::uint32_t time, std::uint16_t offset, std::uint8_t value)
	{
		RegisterWrite write;
		write.reg = (std::uint8_t) (offset & 0xFF);
		write.port = (std::uint8_t) (offset >> 8);
		write.value = value;
		write.sampleTime = time;
		stream.writes.push_back (write);
	}

	/** Volume, end, pitch and the current address, then the control byte that starts the voice. */
	void startSegaVoice (Stream& stream, std::uint32_t time, int channel, std::uint32_t start,
						 std::uint8_t pitch, std::uint8_t endReg, std::uint8_t control = 0x02)
	{
		const std::uint16_t low = (std::uint16_t) (channel * 8);
		const std::uint16_t high = (std::uint16_t) (0x80 + channel * 8);
		segaWrite (stream, time, (std::uint16_t) (low + 2), 0x7F);		// volume left
		segaWrite (stream, time, (std::uint16_t) (low + 3), 0x7F);		// volume right
		segaWrite (stream, time, (std::uint16_t) (low + 6), endReg);
		segaWrite (stream, time, (std::uint16_t) (low + 7), pitch);
		// the 24-bit position's low byte is the chip's own fraction, so the byte address is
		// `(high << 8) | mid`: a start of 0x4000 is mid 0x00 and high 0x40
		segaWrite (stream, time, (std::uint16_t) (high + 4), (std::uint8_t) (start & 0xFF));
		segaWrite (stream, time, (std::uint16_t) (high + 5), (std::uint8_t) ((start >> 8) & 0xFF));
		segaWrite (stream, time, (std::uint16_t) (high + 6), control);
	}

	/** At 4.03 MHz a pitch of 29 advances 3563 bytes a second, so a 16-byte cycle is A3. The end
		register counts 256-byte units: `0x47` ends the sample at `0x4800`, two kilobytes in. */
	void testSegaPcmNote (void)
	{
		Stream stream;
		startSegaVoice (stream, 0, 2, 0x4000, 29, 0x47);

		const auto notes = runSegaPcm (stream, 60000, ga20Square (0x10000, 0x4000, 16, 2048));

		check (notes.size() == 1, "a Sega PCM key-on with a periodic sample is one note");
		if (notes.empty())
		{
			return;
		}

		check (notes[0].logicalChannel == 2, "Sega PCM voice 2 is its own track");
		check (absoluteNote (notes[0]) == 57, "a 16-byte cycle at 3563 bytes a second is A3");
		check (! notes[0].isDrum, "a sample with a period is melodic");
		check (notes[0].channelKind == toybox::chips::ChannelKind::segaPcm, "and its family says so");
		check (notes[0].velocity == 127, "a full-scale volume register is full velocity");
		check (distance (notes[0].endSample, 25350) <= 200,
			   "the end register ends it: 2048 bytes at 3563 a second");
	}

	/** The pitch register is a step: doubling it is an octave, which is what makes the same ROM
		sample a bass line instead of a single note. */
	void testSegaPcmPitchIsAStep (void)
	{
		Stream stream;
		startSegaVoice (stream, 0, 2, 0x4000, 58, 0x47);

		const auto notes = runSegaPcm (stream, 60000, ga20Square (0x10000, 0x4000, 16, 2048));

		check (notes.size() == 1 && absoluteNote (notes[0]) == 69,
			   "twice the pitch is an octave up: A4 from the same sample");
	}

	/** Control bit 0 is the key: clear plays, set stops. A looping voice has no end to wait for, so
		that write is the only note-off there is. */
	void testSegaPcmKeyOff (void)
	{
		Stream stream;
		startSegaVoice (stream, 0, 2, 0x4000, 29, 0x47, 0x00);		// loops
		segaWrite (stream, 1000, (std::uint16_t) (0x80 + 2 * 8 + 6), 0x01);	// off

		const auto notes = runSegaPcm (stream, 60000, ga20Square (0x10000, 0x4000, 16, 2048));

		check (notes.size() == 1 && distance (notes[0].endSample, 1000) <= 64,
			   "setting the control bit ends a looping Sega PCM note");
	}

	/** Drums share these sixteen voices, so a sample with no period stays the C4 trigger. */
	void testSegaPcmPercussion (void)
	{
		std::vector<std::uint8_t> rom (0x10000, 0);
		std::uint32_t seed = 135792468u;
		for (std::size_t i = 0; i < 2048; ++i)
		{
			seed = seed * 1103515245u + 12345u;
			rom[0x5000 + i] = (std::uint8_t) (seed >> 16);
		}

		Stream stream;
		startSegaVoice (stream, 0, 3, 0x5000, 29, 0x57);

		const auto notes = runSegaPcm (stream, 60000, rom);

		check (notes.size() == 1 && absoluteNote (notes[0]) == 60 && notes[0].isDrum,
			   "a Sega PCM sample with no period is the C4 drum trigger");
	}

	int main (int argc, char** argv)
{
	// `--ftm <file>` parses a real module and prints what came out. The unit test can only build a
	// synthetic module, so this is how the seven files in test_file/ftm get checked.
	if (argc >= 3 && std::string (argv[1]) == "--ftm")
	{
		std::ifstream file (argv[2], std::ios::binary);
		const std::vector<char> bytes ((std::istreambuf_iterator<char> (file)),
									   std::istreambuf_iterator<char>());

		toybox::midi::NoteStream stream;
		std::string error;
		const bool ok = toybox::formats::ftm::loadNoteStream (bytes.data(), bytes.size(), stream, error);
		std::printf ("ftm %s ok=%d notes=%d tracks=%d frames/s=%.2f %s\n", argv[2], (int) ok,
					 (int) stream.notes.size(), (int) stream.tracks.size(), stream.tickRate,
					 ok ? "" : error.c_str());
		// The instrument names prove the block was walked correctly: they are ASCII, so an offset
		// error of one byte turns them into noise.
		int lowVelocity = 127;
		int highVelocity = 0;
		for (const auto& note : stream.notes)
		{
			lowVelocity = std::min (lowVelocity, (int) note.note.velocity);
			highVelocity = std::max (highVelocity, (int) note.note.velocity);
		}
		std::printf ("    velocity=%d..%d\n", lowVelocity, highVelocity);

		toybox::formats::ftm::Module module;
		std::string ignored;
		if (ok && toybox::formats::ftm::parseModule (bytes.data(), bytes.size(), module, ignored))
		{
			std::printf ("    instruments=%d", (int) module.instrumentNames.size());
			for (std::size_t i = 0; i < module.instrumentNames.size() && i < 4; ++i)
			{
				std::printf (" [%d] %s vol=%d", (int) i, module.instrumentNames[i].c_str(),
							 module.instrumentVolume[i]);
			}
			std::printf ("\n");
		}

		return ok ? 0 : 1;
	}

	std::printf ("chip decoder test\n");

	// Isolated settings: a decoder test reads none, but the store must never open this machine's
	// own file from a test either (see TestSettings.h for the failure that taught us).
	const toybox::test::ScopedSettingsDirectory settings;

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
	testOpnThreeChannels();
	testOpnaAdpcmOneShots();
	testOpnAdpcmBPitch();
	testOpnAdpcmBDeltaIsThePitch();
	testOpnAdpcmBRealDeltaScale();
	testOpnAdpcmBLevelEndsTheNote();
	testOpnAdpcmBPercussion();
	testOpnAdpcmBAddressShift();
	testOpnbFourChannelVariant();

	testOpmPitchAndKey();
	testOpmClockScaling();

	testOpl2PitchAndKey();
	testOpllPitch();
	testOpl3SecondBank();
	testOplRhythm();

	testGbPulseNote();
	testGbLengthCounterEndsTheNote();
	testGbEnvelopeDecaysTheNoteAway();
	testGbWaveChannel();
	testGbNoiseIsADrum();
	testGbSilenceWithoutPower();

	testC6280WaveNote();
	testC6280SelectsOneChannel();
	testC6280NoiseIsADrum();
	testC6280NoiseOnlyOnTheLastTwoChannels();

	testSccWaveNote();
	testSccKeyMaskEndsTheNote();
	testSccLatchPicksTheChannel();
	testSccSilentWithoutVolume();

	testSaaSquareNote();
	testSaaOctaveIsAnOctave();
	testSaaNeedsTheChipEnable();
	testSaaNoiseOnlyChannelIsADrum();
	testSaaNoiseRatePicksTheDrumKey();

	testPokeySquareNote();
	testPokeyClockModeChangesThePitch();
	testPokeyJoinedPairIsOneVoice();
	testPokeyVolumeOnlyIsSilent();

	testWswanWaveNote();
	testWswanEnableBitEndsTheNote();
	testWswanSilentWithoutVolume();
	testWswanNoiseIsADrum();
	testWswanNoiseRatePicksTheDrumKey();
	testWswanSweepMovesThePitch();
	testWswanVoiceIsAOneShot();

	testSnToneNote();
	testSnDividerNeedsBothBytes();
	testSnNoiseIsADrum();
	testSnZeroDividerIsSilent();

	testAyToneNote();
	testAyMixerBitEndsTheNote();
	testAySilentAmplitude();

	testRf5cChannelIsAOneShot();
	testRf5cMaskSelectsTheChannel();
	testRf5cMasterSwitchEndsTheNote();

	testX1WaveNote();
	testX1DividerHalvesTheFrequency();
	testX1PcmIsAOneShot();
	testX1EnvelopeWritesDoNotKey();

	testYmzChannelIsAOneShot();
	testYmzLatchSelectsTheVoice();
	testYmzSilentLevelIsNoNote();

	testOpxFmNote();
	testOpxAlgorithmDecidesTheCarrier();
	testOpxGroupLevelMutes();
	testOpxBlockIsAnOctave();
	testOpxSyncOneCopiesToThePair();
	testOpxPcmIsAOneShot();
	testOpxPcmBanksAreIndependent();

	testC140Note();
	testC140EndAddressSetsTheLength();
	testC140EndAddressIsLatchedAtKeyOn();
	testC140LoopRunsUntilKeyOff();
	testC140ClockDoublesThePitch();
	testC140SilentVoiceIsNoNote();
	testC140HighVoiceNeedsTheHighOffsetByte();
	testC140TranspositionAndRoot();
	testC140RootIsStableAcrossRates();
	testC140WithoutRomKeepsTheRootRate();
	testC140NoisySampleFallsBackToTheRootRate();
	testC140System2BankIsFolded();

	testC352Note();
	testC352KeyOnIsTheExecuteWrite();
	testC352EndAddressSetsTheLength();
	testC352KeyOffEndsALoopingVoice();
	testC352SilentVoiceIsNoNote();
	testC352RegistersAreSixteenBits();
	testC352SamplePeriodIsTheNote();
	testC352WithoutRomKeepsTheRootRate();
	testC352NoisySampleFallsBackToTheRootRate();
	testC352BankIsPartOfTheAddress();
	testC352WavetableCycle();
	testC352HighVoices();

	testK053260Note();
	testK053260KeyOnIsARisingEdge();
	testK053260LengthSetsTheDuration();
	testK053260LoopRunsUntilKeyOff();
	testK053260SilentVoiceIsNoNote();
	testK053260PercussionIsATrigger();
	testK053260UnmusicalPeriodIsATrigger();
	testK053260KadpcmIsMeasured();
	testK053260Voices();

	testK054539Note();
	testK054539MarkerSetsTheLength();
	testK054539VolumeAttenuates();
	testK054539LoopRunsUntilKeyOff();
	testK054539SilentVoiceIsNoNote();
	testK054539SixteenBitPcm();
	testK054539FourBitDpcm();
	testK054539SmoothAttackIsADrum();
	testK054539RetriggerIsANewNote();
	testK054539Voices();

	testQSoundNote();
	testQSoundRateIsTheKey();
	testQSoundLoopIsALength();
	testQSoundRateSlideIsNotANewNote();
	testQSoundRomBeyondFourMegabytes();
	testQSoundPercussion();
	testQSoundBankBelongsToTheNextVoice();
	testQSoundSilentVoiceIsNoNote();
	testQSoundChannelsAndAdpcm();

	testGa20Note();
	testGa20KeyOff();
	testGa20Percussion();
	testGa20AddressUnit();




	testSampledNoteHelpers();

	testPitchFieldsContract();


	testMikeyNotes();

	testMsm5232Notes();

	testVsuNotes();

	testK005289Notes();

	testOkim6295Notes();
	testOkim6295FourthVoice();
	testOkim6295Unpitched();

	testEs5503Note();
	testEs5503TableCycle();
	testEs5503KeyOff();
	testEs5503OneShot();
	testEs5503Percussion();
	testEs5503OscillatorCount();

	testSegaPcmNote();
	testSegaPcmPitchIsAStep();
	testSegaPcmKeyOff();
	testSegaPcmPercussion();

	// the format loaders live in their own translation unit now
	toybox::test::runFormatTests();

	return toybox::test::report();
}
