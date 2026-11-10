/*
    The NSF header reader and the address space built on top of it.

    Both are cheap to test on their own and expensive to debug once a 6502 is running: a wrong
    offset in the header or a bank window that reads from the copied image instead of the file
    produces silence, not a crash, several layers later.
*/

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

#include "TestSupport.h"
#include "chips/NesDpcmSample.h"
#include "backends/libvgm/Ym2612DacSampler.h"
#include "backends/nsf/NsfBus.h"
#include "backends/nsf/NsfPlayer.h"
#include "playback/WaveShape.h"
#include "formats/nsf/NsfFile.h"
#include "xgm/devices/Sound/nes_apu.h"
#include "xgm/devices/Sound/nes_dmc.h"

namespace
{

using toybox::formats::nsf::NsfFile;

/** Builds an image with the fields this reader is supposed to find. */
std::vector<uint8_t> makeNsf (int version = 1, int songs = 3, int start = 2,
								   uint16_t ntsc = 16666, uint8_t chips = 0x01,
								   std::size_t programBytes = 0x100,
								   uint8_t extraChips = 0)
{
	std::vector<uint8_t> image (NsfFile::headerSize + programBytes, 0);

	std::memcpy (image.data(), "NESM", 4);
	image[0x04] = 0x1A;
	image[NsfFile::offsetVersion] = (uint8_t) version;
	image[NsfFile::offsetTotalSongs] = (uint8_t) songs;
	image[NsfFile::offsetStartSong] = (uint8_t) start;

	const auto put16 = [&image] (std::size_t at, uint16_t value)
	{
		image[at] = (uint8_t) (value & 0xFF);
		image[at + 1] = (uint8_t) (value >> 8);
	};

	put16 (NsfFile::offsetLoadAddress, version == 1 ? 0x8000 : 0x6000);
	put16 (NsfFile::offsetInitAddress, 0x8123);
	put16 (NsfFile::offsetPlayAddress, 0x8456);
	put16 (NsfFile::offsetNtscSpeed, ntsc);
	put16 (NsfFile::offsetPalSpeed, 19997);

	std::memcpy (image.data() + NsfFile::offsetName, "A title", 7);
	std::memcpy (image.data() + NsfFile::offsetArtist, "An artist", 9);
	std::memcpy (image.data() + NsfFile::offsetCopyright, "2026", 4);

	image[NsfFile::offsetChipFlags] = chips;
	image[NsfFile::offsetExtraChipFlags] = extraChips;

	for (std::size_t i = 0; i < programBytes; ++i)
	{
		image[NsfFile::headerSize + i] = (uint8_t) (0xEA); // NOP
	}

	return image;
}

} // namespace

int main (void)
{
	std::printf ("[1] the NSF header\n");

	{
		const auto image = makeNsf();
		NsfFile file;
		std::string error;

		check (NsfFile::parse (image.data(), image.size(), file, error), "an NSF image is read");
		check (NsfFile::looksLikeNsf (image.data(), image.size()), "and is recognised by its magic");
		check (file.totalSongs == 3 && file.startSong == 2, "the song count and start are as written");
		check (file.initAddress == 0x8123 && file.playAddress == 0x8456, "with its two entry points");
		check (file.loadAddress == 0x8000, "the load address comes from the field");
		check (file.name == "A title" && file.artist == "An artist", "and its names survive");
		check (file.copyright == "2026", "including the copyright");

		// The field is the load address in version 1 too. Entry025.nsf declares $8AFF and its
		// driver is assembled for that address: forcing $8000 executes somebody else's bytes.
		{
			auto unaligned = makeNsf (1, 1, 1, 16639, 0, 0x100);
			unaligned[NsfFile::offsetLoadAddress] = 0xFF;
			unaligned[NsfFile::offsetLoadAddress + 1] = 0x8A;

			NsfFile other;
			std::string otherError;
			check (NsfFile::parse (unaligned.data (), unaligned.size (), other, otherError),
				   "a version 1 image with an unaligned load address parses");
			check (other.loadAddress == 0x8AFF, "and keeps the address it declares");
		}

		// Japanese titles are Shift-JIS; the card needs UTF-8.
		{
			auto japanese = makeNsf ();
			const unsigned char title[] = { 0x31, 0x39, 0x38, 0x33, 0x94, 0x4E, 0x82, 0xCC };  // 1983年

			std::memcpy (japanese.data () + NsfFile::offsetName, title, sizeof (title));
			japanese[NsfFile::offsetName + sizeof (title)] = 0;

			NsfFile other;
			std::string otherError;
			check (NsfFile::parse (japanese.data (), japanese.size (), other, otherError),
				   "an image with a Japanese title parses");
			// "1983" stays as it is, and the two Shift-JIS characters become three UTF-8 bytes
			// each, starting with a lead byte - the exact code points belong to the decoder, not
			// to this test.
			const bool converted = other.name.size () == 10
								   && other.name.compare (0, 4, "1983") == 0
								   && (unsigned char) other.name[4] == 0xE5
								   && (unsigned char) other.name[7] == 0xE3;

			check (converted, "and its title is converted to UTF-8");
		}
		check (file.chipFlags == 0x01 && file.chip() == toybox::formats::nsf::NsfChip::vrc6,
			   "the chip byte names the expansion");
		check (! file.isPal(), "an even speed is NTSC");
		check (file.rom.size() == 0x100, "the program is everything after the header");
	}

	{
		// Every VRC6 and VRC7 file in test_file leaves 0x7A at zero and names its chip in 0x7B, the
		// NSF 2.0 byte. A reader that only looked at 0x7A played them as a bare 2A03 - and the
		// hand-assembled assets could not catch it, because they write the flag where the format
		// puts it and those files do not.
		const auto extended = makeNsf (1, 1, 1, 16666, 0x00, 0x100, 0x02);
		NsfFile file;
		std::string error;

		check (NsfFile::parse (extended.data(), extended.size(), file, error),
			   "an image with the chip in the extended byte parses");
		check (file.chip() == toybox::formats::nsf::NsfChip::vrc7,
			   "the extended byte names the expansion when the first one is empty");

		// And 0x7A still wins where both are set: it is where the format puts the flags, and a
		// reader that preferred the extension byte would be guessing.
		const auto both = makeNsf (1, 1, 1, 16666, 0x01, 0x100, 0x02);
		NsfFile second;

		check (NsfFile::parse (both.data(), both.size(), second, error)
				 && second.chip() == toybox::formats::nsf::NsfChip::vrc6,
			   "the format byte wins when both are set");
	}

	{
		const auto image = makeNsf (2);
		NsfFile file;
		std::string error;

		check (NsfFile::parse (image.data(), image.size(), file, error), "a version 2 image reads too");
		check (file.loadAddress == 0x6000, "and its load address comes from the field");
	}

	{
		const auto image = makeNsf (1, 1, 1, 16639 | 1);
		NsfFile file;
		std::string error;

		check (NsfFile::parse (image.data(), image.size(), file, error), "an image with the PAL bit reads");
		check (file.isPal(), "the lowest bit of the speed word is the PAL flag");
		check (file.framesPerSecond() > 60.0 && file.framesPerSecond() < 60.2,
			   "16639 microseconds is 60.1 frames per second");
	}

	{
		const auto image = makeNsf (1, 1, 1, 0);
		NsfFile file;
		std::string error;

		NsfFile::parse (image.data(), image.size(), file, error);
		check (file.framesPerSecond() > 60.0 && file.framesPerSecond() < 60.2,
			   "a zero speed falls back to the documented NTSC default");
	}

	{
		std::vector<uint8_t> shortImage (16, 0);
		NsfFile file;
		std::string error;

		check (! NsfFile::parse (shortImage.data(), shortImage.size(), file, error),
			   "an image shorter than a header is refused");
		check (! error.empty(), "with a reason");
	}

	{
		auto image = makeNsf();
		image[0] = 'X';
		NsfFile file;
		std::string error;

		check (! NsfFile::parse (image.data(), image.size(), file, error),
			   "so is one without the magic");
	}

	std::printf ("[2] the address space\n");

	{
		const auto image = makeNsf();
		NsfFile file;
		std::string error;
		NsfFile::parse (image.data(), image.size(), file, error);

		toybox::backends::nsf::NsfBus bus (file);
		bus.load();

		int writes = 0;
		uint16_t lastAddress = 0;
		bus.onSoundWrite = [&] (uint16_t address, uint8_t)
		{
			++writes;
			lastAddress = address;
		};

		uint32_t value = 0;
		bus.Write (0x4015, 0x0F);
		check (writes == 1 && lastAddress == 0x4015, "a sound-register write reaches the player");
		check (bus.soundWriteCount == 1, "and is counted");

		bus.Read (0x4015, value);
		check (value == 0, "and cannot be read back - the register is not memory");

		bus.Write (0x0000, 0x42);
		bus.Read (0x0800, value);
		check (value == 0x42, "RAM still mirrors every two kilobytes");

		// The trampoline has to be there after load(), not before: SetImage clears the image.
		bus.Read (0x4100, value);
		const bool first = value == 0x20;
		bus.Read (0x4103, value);
		check (first && value == 0x4C, "the player trampoline survives the image load");
		check (writes == 1, "and installing it writes no sound register");

		// --- what the DPCM's registers said when the sample started -------------------------------
		//
		// Nothing on the chip reports the length back: NES_DMC hands out the address and the rate
		// through its track info and stops there, and a file written from a sample has to know where
		// it ends. So the bus remembers the three writes and latches them at the one that starts the
		// sample - a driver is free to point $4012 and $4013 at the next sample while the current
		// one is still playing, and the registers would then describe something nobody hears.
		check (! bus.dpcm.valid, "nothing is latched before a sample starts");

		bus.Write (0x4012, 0x03);
		bus.Write (0x4013, 0x02);
		check (! bus.dpcm.valid, "and pointing at one does not start it either");

		bus.Write (0x4015, 0x10);
		check (bus.dpcm.valid, "the write that starts the sample latches it");
		check (bus.dpcm.start == 0xC0C0, "at $C000 plus the page, sixty-four bytes each");
		check (bus.dpcm.length == 33, "with the length in sixteen-byte units, plus that one byte");

		// All eight bits of the length register count, which is what the core does -
		// `dlength = (len_reg << 4) + 1` from the full byte - and what NotSoFatso does at
		// NSF_Core.cpp:321. 0x2B is 689 bytes, and a snare really is that long.
		//
		// Reading only the low four bits, the real 2A03's width, is what made a 689-byte sample
		// come out as 177: a snare that sounded like a click. The address register is different -
		// it is `$C000 | (value << 6)`, so its top two bits fall off by construction.
		bus.Write (0x4015, 0x00);			// stops it, which is what arms the next one
		bus.Write (0x4012, 0x07);
		bus.Write (0x4013, 0x2B);
		bus.Write (0x4015, 0x10);
		check (bus.dpcm.start == 0xC1C0, "the address is $C000 plus the page");
		check (bus.dpcm.length == 689, "and the length counts all eight bits of its register");

		// Only the 0 -> 1 edge starts a sample. The failure this catches is the one this song had:
		// a driver that leaves the bit set and writes $4015 for the pulse channels every frame
		// latched the registers on every one of those writes, and the pair it caught was a page of
		// silence rather than the drum.
		bus.Write (0x4012, 0x09);
		bus.Write (0x4015, 0x10);
		check (bus.dpcm.start == 0xC1C0, "a set bit written again latches nothing new");

		// The byte a sample is drawn from has to be the byte the CPU would read: same bank mapping,
		// same image. peek() takes the same path by construction, so this checks that it does.
		uint32_t cpuSaw = 0;
		bus.Read (0xC1C0, cpuSaw);
		check (bus.peek (0xC1C0) == (uint8_t) (cpuSaw & 0xFF),
			   "peek answers what the CPU would have been given");

		// The rate travels with the rest, and it has to: the same 193 bytes are 46 ms at the top of
		// the table and 370 ms at the bottom, which is the difference between a click and a kick.
		bus.Write (0x4015, 0x00);
		bus.Write (0x4010, 0x48);			// rate 8, loop on, irq off
		bus.Write (0x4012, 0x0B);
		bus.Write (0x4013, 0x0C);
		bus.Write (0x4015, 0x10);
		check (bus.dpcm.rateIndex == 8, "the rate index is latched with the address");
		check (bus.dpcm.loop, "and so is the loop bit");
	}

	{
		// --- from a register's value to the bytes a file holds ---------------------------------
		//
		// Three steps with no test until now, and every error this feature has had was in one of
		// them: a length read four bits wide, a tail that kept its filler, a padding byte counted
		// as data. Each was found by comparing an exported file against one FamiStudio produced,
		// which is a slow way to learn that a number is off by one.
		//
		// The numbers below are rungs of a ladder rather than observations of one song: 4081 is the
		// register's own ceiling, and no file here reaches it.
		using namespace toybox::backends::nsf;

		check (dpcm::lengthFromRegister (0x00) == 1, "an empty length register still asks for one byte");
		check (dpcm::lengthFromRegister (0x0C) == 193, "twelve units plus the padding byte");
		check (dpcm::lengthFromRegister (0xFF) == dpcm::kMaxBytes,
			   "and the ceiling comes from the register, not from a song");

		// Filler in all four shapes it takes here: silence in DPCM, and the two unwritten patterns.
		const uint8_t trailing[] = { 0x12, 0x34, 0x00, 0x55, 0xAA, 0xFF };
		check (dpcm::trimFiller (trailing, 6) == 2, "trailing filler is not part of the sample");
		check (dpcm::trimFiller (trailing, 2) == 2, "and a tail without any is left alone");

		const uint8_t silence[] = { 0x55, 0xAA, 0x00 };
		check (dpcm::trimFiller (silence, 3) == 0, "a sample of nothing trims to nothing");

		check (dpcm::trimFiller (trailing, 0) == 0, "an empty sample stays empty");
		check (dpcm::trimFiller (nullptr, 8) == 0, "and no buffer is not a crash");

		// The padding byte. 321 is 16*20+1, so its last byte is the one the formula added.
		check (dpcm::dropPaddingByte (321) == 320, "a length on 16k+1 loses the padding byte");
		check (dpcm::dropPaddingByte (320) == 320, "and a length that only lands near it keeps every byte");
		check (dpcm::dropPaddingByte (1) == 0, "one byte is the padding byte and nothing else");
		check (dpcm::dropPaddingByte (0) == 0, "and an empty sample cannot go below empty");

		// Both steps on the shape Entry080's first sample has: 320 bytes of data, then filler, with
		// the register describing one byte more than the file holds.
		std::vector<uint8_t> padded (321, 0x00);

		for (std::size_t i = 0; i < 320; ++i)
		{
			padded[i] = (uint8_t) (i * 7u + 3u);
		}

		check (dpcm::finalLength (321, padded.data ()) == 320,
			   "filler first, then the padding byte, is what the file holds");

		// The integration. Alternating bits walk nowhere, which is why 0xAA and 0x55 are silence,
		// and eight ones in a row climb by exactly eight steps of two.
		const uint8_t alternating[] = { 0x55 };
		const auto flat = dpcm::decode (alternating, 1);

		check (flat.size () == 8, "eight steps per byte");
		check (std::abs (flat[0] + 0.015625f) < 0.0001f, "the first step down is two of a hundred and twenty-eight");

		const uint8_t upward[] = { 0xFF };
		const auto rising = dpcm::decode (upward, 1);

		check (std::abs (rising[7] - 0.125f) < 0.0001f, "eight steps up leave the level at sixteen");

		// The rail is seven bits and asymmetrical: clamping at 127 rather than 128 is what keeps
		// the top of a loud sample from folding, and the divisor matches the range.
		std::vector<uint8_t> loud (9, 0xFF);
		const auto clamped = dpcm::decode (loud.data (), loud.size ());

		check (std::abs (clamped.back () - 127.0f / 128.0f) < 0.0001f, "and the level stops at the positive rail");
	}

	{
		// A banked program: 48 KB, so $8000 cannot hold it and reads must come from the file.
		auto image = makeNsf (1, 1, 1, 16639, 0, 0xC000);

		// Mark each 4 KB bank with its own number at its first byte.
		for (std::size_t bank = 0; bank < 12; ++bank)
		{
			image[NsfFile::headerSize + bank * 0x1000] = (uint8_t) bank;
		}

		NsfFile file;
		std::string error;
		NsfFile::parse (image.data(), image.size(), file, error);

		check (file.isBanked(), "a program larger than the address space is banked");

		toybox::backends::nsf::NsfBus bus (file);
		bus.load();

		uint32_t value = 0;
		bus.Read (0x8000, value);
		check (value == 0, "the first window starts on bank 0");

		bus.Write (0x5FF8, 3);
		bus.Read (0x8000, value);
		check (value == 3, "and a write to $5FF8 switches it to bank 3");

		bus.Write (0x5FFB, 6);
		bus.Read (0xE000, value);
		check (value == 6, "the fourth window follows its own register");
	}

	std::printf ("[3] the chip renders its channels separately\n");

	{
		xgm::NES_APU apu;
		apu.SetClock (1789772.727272727);
		apu.SetRate (44100.0);
		apu.Reset ();
		apu.Write (0x4015, 0x0F);
		apu.Write (0x4000, 0xBF);
		apu.Write (0x4002, 0xFF);
		apu.Write (0x4003, 0x00);
		apu.Write (0x4004, 0xBF);
		apu.Write (0x4006, 0x55);
		apu.Write (0x4007, 0x00);
		apu.Tick (2000);

		int32_t mixed[2] = { 0, 0 };
		apu.Render (mixed);

		int32_t channels[4] = { 0, 0, 0, 0 };
		apu.RenderChannels (channels, 2, 2);

		const int sumLeft = (int) channels[0] + (int) channels[2];
		const int sumRight = (int) channels[1] + (int) channels[3];

		check (std::abs (sumLeft - (int) mixed[0]) <= 2 && std::abs (sumRight - (int) mixed[1]) <= 2,
			   "and their sum is what Render returns");
	}



#if defined (TOYBOX_TEST_DPCM_SONG)

	{
		// --- the same question, asked of a real song -------------------------------------------
		//
		// The arithmetic above is checked with made-up bytes. This walks the whole path on a file:
		// dpcm_two_samples.nsf starts a 320-byte sample at $D200 twelve seconds in, and a 512-byte
		// one at $D380 thirty-five seconds in - the lengths FamiStudio exports for the same song.
		//
		// Four separate faults once hid behind those two numbers: a length read four bits wide, a
		// tail that kept its filler, a padding byte counted as data, and a boundary taken from the
		// wrong register. Every one of them was found by comparing an exported file by hand,
		// because the tests stopped at the arithmetic and this path was only ever walked by a probe
		// someone remembered to run.
		std::FILE* asset = std::fopen (TOYBOX_TEST_DPCM_SONG, "rb");

		check (asset != nullptr, "the DPCM asset opens");

		if (asset != nullptr)
		{
			std::fseek (asset, 0, SEEK_END);
			const auto size = (std::size_t) std::ftell (asset);
			std::fseek (asset, 0, SEEK_SET);

			std::vector<uint8_t> bytes (size);
			const auto read = std::fread (bytes.data (), 1, size, asset);
			std::fclose (asset);

			check (read == size, "and reads whole");

			toybox::formats::nsf::NsfFile song;
			std::string error;

			check (toybox::formats::nsf::NsfFile::parse (bytes.data (), bytes.size (), song, error),
				   "and parses as an NSF");

			toybox::backends::nsf::NsfPlayer player (song, 44100.0);
			player.start ();

			std::vector<float> left (1024, 0.0f), right (1024, 0.0f);

			// Forty seconds, long enough for both samples to have started.
			for (int block = 0; block < 1722; ++block)
			{
				player.render (left.data (), right.data (), 1024);
			}

			// The list an export walks: distinct samples, in the order they first played, already
			// trimmed and decoded. Reading "whatever is playing now" would describe one of the two
			// and say nothing about the other - the mistake this feature kept making in review.
			const auto samples = player.dpcmSamples ();

			check (samples.size () == 2, "the song has two DPCM samples");

			if (samples.size () == 2)
			{
				check (samples[0].name == "dpcm_D200_320",
					   "the first is the 320-byte sample at $D200");
				check (samples[1].name == "dpcm_D380_512",
					   "and the second the 512-byte one at $D380");
				check (samples[0].values.size () == 320u * 8u, "eight points per byte of the sample");
				check (samples[0].rate > 33000.0 && samples[0].rate < 33400.0,
					   "at the rate the sample's own register names");

				// And the writer, into a folder of its own: two files whose sizes are the header
				// plus eight points per byte at four bytes a point. This is the path File > Export
				// takes, minus the folder picker.
				const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
										.getChildFile ("vgmtoybox-dpcm-"
													   + juce::String (juce::Random::getSystemRandom().nextInt (100000)));
				const auto made = folder.createDirectory ();

				check (made.wasOk (), "a temporary folder for the export");

				if (made.wasOk ())
				{
					const auto written = toybox::playback::writeSamples (
						samples, folder.getFullPathName ().toStdString ());

					check (written == 2, "both samples were written");

					const auto first = folder.getChildFile ("dpcm_D200_320.wav");
					const auto second = folder.getChildFile ("dpcm_D380_512.wav");

					check (first.existsAsFile (), "the 320-byte sample is a file with its own name");
					check (second.existsAsFile (), "and the 512-byte one too");

					check (first.getSize () == 44 + 320 * 8 * 4,
						   "and its size is the header plus eight points per byte");

					// **The length limit is the writer's, not a chip's.** Every collector hands over
					// levels and a rate, so one rule covers them all - and a file cut here is what
					// keeps a driver's "end at the end of the ROM" from exporting a bank as a
					// minute-long sample. One millisecond is shorter than both of these.
					int truncated = 0;
					const auto cut = toybox::playback::writeSamples (
						samples, folder.getFullPathName ().toStdString (), 0.001, &truncated);

					check (cut == 2 && truncated == 2,
						   "a length limit cuts every file that exceeds it, and says how many");
					check (first.getSize () < 44 + 320 * 8 * 4,
						   "and the file on disk is shorter afterwards");

					// **A target rate holds the levels there instead of resampling later.** The same
					// bytes written for a 44.1 kHz project: the file needs no conversion on import,
					// which is what keeps an 8-bit DAC's staircase (and its high end) intact.
					const auto heldFolder = folder.getChildFile ("held");

					check (heldFolder.createDirectory ().wasOk (),
						   "a second folder for the held export");

					const auto held = toybox::playback::writeSamples (
						samples, heldFolder.getFullPathName ().toStdString (), 0.0, nullptr, 44100.0);

					check (held == 2, "both samples are written at the target rate too");

					const auto heldFile = heldFolder.getChildFile ("dpcm_D200_320.wav");
					const auto expectedPoints =
						(juce::int64) std::llround (2560.0 * 44100.0 / samples[0].rate);

					check (heldFile.getSize () == 44 + expectedPoints * 4,
						   "and the file is the header plus one point per held step");

					juce::FileInputStream stream (heldFile);
					char header[44] = {};

					check (stream.read (header, 44) == 44, "the held file has a header");

					const auto headerRate = (uint32_t) (uint8_t) header[24]
											| ((uint32_t) (uint8_t) header[25] << 8)
											| ((uint32_t) (uint8_t) header[26] << 16)
											| ((uint32_t) (uint8_t) header[27] << 24);

					check (headerRate == 44100u,
						   "and it declares the rate it was written for, not the chip's");

					// The drag path is a second entry into the writer, and the two must not disagree:
					// a drag that produced a different file from File > Export would be a bug the
					// user hears rather than sees.
					toybox::playback::ChannelState dragged;
					dragged.wave = std::make_shared<const std::vector<float>> (
						std::vector<float> { 0.0f, 0.5f, -0.5f, 0.0f });
					dragged.sampleRate = 8000.0f;

					const auto dragPath = heldFolder.getChildFile ("drag.wav");

					check (toybox::playback::writeSampleWav (
							   dragged, dragPath.getFullPathName ().toStdString (), 44100.0),
						   "a single-channel drag is written at the target rate too");

					const auto dragPoints =
						(juce::int64) std::llround (4.0 * 44100.0 / (double) dragged.sampleRate);

					check (dragPath.getSize () == 44 + dragPoints * 4,
						   "and holds each level at that rate");

					folder.deleteRecursively ();
				}
			}

			// And the path the export actually leans on: walk the song, then ask. Nothing has
			// played, so the player's own list is empty - what comes back is the scan's, which is
			// the whole song's. Before this, the answer depended on how long the user had listened.
			{
				toybox::backends::nsf::NsfPlayer scanner (song, 44100.0);
				auto handle = std::make_shared<toybox::backends::nsf::ScanHandle> ();
				scanner.setScanHandle (handle);
				scanner.scanSong ();

				const auto walked = scanner.dpcmSamples ();

				check (walked.size () == 2, "walking the song finds both samples without playing");
				check (walked.size () == 2 && walked[0].name == "dpcm_D200_320",
					   "and names them the same way the playing list does");
			}
		}
	}

#endif

	// writeChannelShape: one channel, three answers. The drag feature refused to start on
	// `nothingToWrite` and offered to explain `failed`, which is why they are separate answers
	// rather than a bool.
	{
		const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
								.getChildFile ("vgmtoybox-shape-"
											   + juce::String (juce::Random::getSystemRandom().nextInt (100000)));
		const auto made = folder.createDirectory ();

		check (made.wasOk (), "a folder for the shape writer");

		if (made.wasOk ())
		{
			const auto path = folder.getChildFile ("probe.wav").getFullPathName ().toStdString ();

			toybox::playback::ChannelState pulse;
			pulse.name = "Pulse 1";
			pulse.chipName = "2A03";
			pulse.waveKind = 2;				// pulse: a period the panel draws from duty
			// The wave is shared rather than held by value, so a test that supplies one owns it the
			// same way a decoder does.
			pulse.wave = std::make_shared<const std::vector<float>> (
				std::vector<float> { -1.0f, -1.0f, 1.0f, 1.0f });

			check (toybox::playback::hasSomethingToWrite (pulse),
				   "a pulse channel has something to write");
			check (toybox::playback::writeChannelShape (pulse, path)
					   == toybox::playback::ShapeWrite::written,
				   "and writing it succeeds");

			// The same channel with a rate is a recording: the rate is what makes it bytes.
			auto sampled = pulse;
			sampled.waveKind = 5;			// delta
			sampled.sampleRate = 33143.9f;

			check (toybox::playback::writeChannelShape (sampled, path)
					   == toybox::playback::ShapeWrite::written,
				   "a sampled channel is written as audio");

			toybox::playback::ChannelState empty;
			empty.name = "PCM";
			empty.chipName = "MMC5";

			check (! toybox::playback::hasSomethingToWrite (empty),
				   "a channel with no kind and no bytes has nothing");
			check (toybox::playback::writeChannelShape (empty, path)
					   == toybox::playback::ShapeWrite::nothingToWrite,
				   "and writing it says so rather than reporting a failure");

			folder.deleteRecursively ();
		}
	}

	// The drag cache's names: a timestamp a person can read, and a suffix rather than an overwrite
	// when two drags land inside one second.
	{
		std::tm when = {};
		when.tm_year = 126;			// 2026, counted from 1900
		when.tm_mon = 10;			// November, counted from zero
		when.tm_mday = 1;
		when.tm_hour = 8;
		when.tm_min = 23;
		when.tm_sec = 56;

		std::vector<std::string> taken;

		const auto exists = [&taken] (const std::string& name)
		{
			return std::find (taken.begin (), taken.end (), name) != taken.end ();
		};

		const auto first = toybox::playback::dragExportName ("2A03_Pulse 1", when, exists);
		check (first == "2A03_Pulse 1_2026_1101_082356.wav",
			   "the name carries a timestamp a person can read");

		taken.push_back (first);

		const auto second = toybox::playback::dragExportName ("2A03_Pulse 1", when, exists);
		check (second == "2A03_Pulse 1_2026_1101_082356-2.wav",
			   "a second drag in the same second takes a suffix instead of the first file");

		taken.push_back (second);

		check (toybox::playback::dragExportName ("2A03_Pulse 1", when, exists)
				   == "2A03_Pulse 1_2026_1101_082356-3.wav",
			   "and the third numbers again");

		check (toybox::playback::dragExportName ("N163_Wave 8", when, exists)
				   == "N163_Wave 8_2026_1101_082356.wav",
			   "a different channel keeps the plain name");

		// The loop stops at a bound rather than at infinity, so there is a case where every name it
		// tried was taken. That has to answer "none" rather than hand back a name that exists: the
		// caller would write over a file the user still has.
		const auto full = [] (const std::string&) { return true; };

		check (toybox::playback::dragExportName ("2A03_Pulse 1", when, full).empty (),
			   "when no name is free the answer is no name");
	}

	// The cleaner: old wavs go, recent wavs stay, and nothing that is not a wav is touched. The
	// last of those is the one that matters - this runs against a temp folder.
	{
		const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
								.getChildFile ("vgmtoybox-cache-"
											   + juce::String (juce::Random::getSystemRandom().nextInt (100000)));

		check (folder.createDirectory ().wasOk (), "a folder that stands in for the cache");

		const auto oldFile = folder.getChildFile ("old.wav");
		const auto newFile = folder.getChildFile ("new.wav");
		const auto other = folder.getChildFile ("notes.txt");

		oldFile.create ();
		newFile.create ();
		other.create ();

		namespace fs = std::filesystem;

		const auto now = fs::file_time_type::clock::now ();

		// Timestamps are set rather than waited for. Same clock as the cleaner uses, which is what
		// makes this deterministic: a wall clock would make "five hours ago" depend on the machine.
		fs::last_write_time (fs::path (oldFile.getFullPathName ().toRawUTF8 ()), now - std::chrono::hours (5));
		fs::last_write_time (fs::path (newFile.getFullPathName ().toRawUTF8 ()), now - std::chrono::hours (1));
		fs::last_write_time (fs::path (other.getFullPathName ().toRawUTF8 ()), now - std::chrono::hours (5));

		const auto removed = toybox::playback::clearDragCache (
			folder.getFullPathName ().toStdString (), 3.0 * 3600.0);

		check (removed == 1, "the cleaner removes only the wav past its retention");
		check (! oldFile.existsAsFile (), "the five-hour-old wav is gone");
		check (newFile.existsAsFile (), "the one-hour-old wav is kept");
		check (other.existsAsFile (), "and a file that is not a wav is never touched");

		folder.deleteRecursively ();
	}

	/**
		**The bank table decides how the image is addressed, not its size.**

		Ai Senshi Nicol is 20 KB of program with `init` at $D02B: that address exists only when the
		$D000 window shows bank 0. Judging from the size alone mapped it linearly, the CPU read past
		the end of the image and executed zero page - one DC level, no register writes at all. The
		header says which it is, and that is what this asserts.
	*/
	{
		auto plain = makeNsf ();
		NsfFile linear;
		std::string error;

		check (NsfFile::parse (plain.data (), plain.size (), linear, error), "an image without banks parses");
		check (! linear.isBanked (), "and is addressed linearly");

		auto declared = makeNsf ();
		declared[NsfFile::offsetBankSwitch] = 0x03;		// one window mapped, the rest unused

		NsfFile banked;

		check (NsfFile::parse (declared.data (), declared.size (), banked, error), "an image with a bank table parses");
		check (banked.isBanked (), "a single declared window makes a 256-byte image banked");
	}

	/**
		**Which song a request lands on.** The header names one, a caller may ask for another, and a
		request outside the file has to fall back rather than index past the table the driver keeps.
		This mapping lived in the backend until now, where only a player could reach it.
	*/
	{
		auto image = makeNsf (1, 34, 16);
		NsfFile chosen;
		std::string error;

		check (NsfFile::parse (image.data (), image.size (), chosen, error), "a 34-song image parses");
		check (chosen.startSongFor (0) == 0, "a player may start at the first song");
		check (chosen.startSongFor (33) == 33, "and at the last one");
		check (chosen.startSongFor (34) == 15, "one past the end falls back to the header's choice");
		check (chosen.startSongFor (-1) == 15, "so does a negative request");
	}

	/**
		**Which DAC hit a row is showing.** The lookup is "the hit that is playing, or the last one that
		played", and it is the only piece of the row's behaviour that is not a decode: the boundaries
		come from the exporter's sampler, the bytes from the bank, and this decides between them.
	*/
	{
		using toybox::backends::libvgm::dac::hitAt;
		using toybox::backends::libvgm::dac::Run;

		const std::vector<Run> runs = { Run { 100, 250, 0, 10, 150 },
										Run { 400, 500, 100, 20, 100 } };

		check (hitAt (runs, 50) == nullptr, "nothing has played before the first hit");
		check (hitAt (runs, 150) == &runs[0], "the first hit is showing while it plays");
		check (hitAt (runs, 300) == &runs[0],
			   "and it stays after it ends - a stopped row keeps the drum it last played");
		check (hitAt (runs, 450) == &runs[1], "the second hit takes over when it starts");
		check (hitAt (runs, 9000) == &runs[1], "and the last one stays on screen at the end");
	}

	/**
		**Which note a DAC sample gets.** The row used to say C4 for every hit, which cannot tell a
		kick from a snare; the sample's index now names the note, and the index is the export's own
		numbering so the note and the file name agree.
	*/
	{
		using toybox::backends::libvgm::dac::assignSampleIndices;
		using toybox::backends::libvgm::dac::kNoSample;
		using toybox::backends::libvgm::dac::noteForSample;
		using toybox::backends::libvgm::dac::Run;

		std::vector<Run> runs = { Run { 0, 10, 0, 4, 10 },
								  Run { 20, 30, 100, 4, 10 },
								  Run { 40, 50, 0, 4, 10 },
								  Run { 60, 70, 200, 1, 10 } };

		assignSampleIndices (runs, 1000);

		check (runs[0].sample == 0, "the first distinct run is sample 0");
		check (runs[1].sample == 1, "a run at another offset is the next sample");
		check (runs[2].sample == 0, "the same bytes played again keep their number");
		check (runs[3].sample == kNoSample, "a run shorter than two bytes is not a sample");

		check (noteForSample (0) == 60, "sample 0 is C4");
		check (noteForSample (7) == 67, "sample 7 is G4");
		check (noteForSample (60) == 60, "the numbering wraps after five octaves");
		check (noteForSample (61) == 61, "and continues from C4 again");
	}

	std::printf ("\nResult    : %s\n", toybox::test::failures() == 0 ? "all checks passed" : "FAILURES");

	return toybox::test::failures() == 0 ? 0 : 1;
}
