/*
    The NSF header reader and the address space built on top of it.

    Both are cheap to test on their own and expensive to debug once a 6502 is running: a wrong
    offset in the header or a bank window that reads from the copied image instead of the file
    produces silence, not a crash, several layers later.
*/

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

#include "TestSupport.h"
#include "backends/nsf/NsfBus.h"
#include "formats/nsf/NsfFile.h"
#include "xgm/devices/Sound/nes_apu.h"
#include "xgm/devices/Sound/nes_dmc.h"

namespace
{

using toybox::formats::nsf::NsfFile;

/** Builds an image with the fields this reader is supposed to find. */
std::vector<std::uint8_t> makeNsf (int version = 1, int songs = 3, int start = 2,
								   std::uint16_t ntsc = 16666, std::uint8_t chips = 0x01,
								   std::size_t programBytes = 0x100)
{
	std::vector<std::uint8_t> image (NsfFile::headerSize + programBytes, 0);

	std::memcpy (image.data(), "NESM", 4);
	image[0x04] = 0x1A;
	image[NsfFile::offsetVersion] = (std::uint8_t) version;
	image[NsfFile::offsetTotalSongs] = (std::uint8_t) songs;
	image[NsfFile::offsetStartSong] = (std::uint8_t) start;

	const auto put16 = [&image] (std::size_t at, std::uint16_t value)
	{
		image[at] = (std::uint8_t) (value & 0xFF);
		image[at + 1] = (std::uint8_t) (value >> 8);
	};

	put16 (NsfFile::offsetLoadAddress, version == 1 ? 0x1234 : 0x6000);
	put16 (NsfFile::offsetInitAddress, 0x8123);
	put16 (NsfFile::offsetPlayAddress, 0x8456);
	put16 (NsfFile::offsetNtscSpeed, ntsc);
	put16 (NsfFile::offsetPalSpeed, 19997);

	std::memcpy (image.data() + NsfFile::offsetName, "A title", 7);
	std::memcpy (image.data() + NsfFile::offsetArtist, "An artist", 9);
	std::memcpy (image.data() + NsfFile::offsetCopyright, "2026", 4);

	image[NsfFile::offsetChipFlags] = chips;

	for (std::size_t i = 0; i < programBytes; ++i)
	{
		image[NsfFile::headerSize + i] = (std::uint8_t) (0xEA); // NOP
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
		check (file.loadAddress == 0x8000, "version 1 always loads at $8000, whatever the field says");
		check (file.name == "A title" && file.artist == "An artist", "and its names survive");
		check (file.copyright == "2026", "including the copyright");
		check (file.chipFlags == 0x01 && file.chip() == toybox::formats::nsf::NsfChip::vrc6,
			   "the chip byte names the expansion");
		check (! file.isPal(), "an even speed is NTSC");
		check (file.rom.size() == 0x100, "the program is everything after the header");
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
		std::vector<std::uint8_t> shortImage (16, 0);
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
		std::uint16_t lastAddress = 0;
		bus.onSoundWrite = [&] (std::uint16_t address, std::uint8_t)
		{
			++writes;
			lastAddress = address;
		};

		UINT32 value = 0;
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
	}

	{
		// A banked program: 48 KB, so $8000 cannot hold it and reads must come from the file.
		auto image = makeNsf (1, 1, 1, 16639, 0, 0xC000);

		// Mark each 4 KB bank with its own number at its first byte.
		for (std::size_t bank = 0; bank < 12; ++bank)
		{
			image[NsfFile::headerSize + bank * 0x1000] = (std::uint8_t) bank;
		}

		NsfFile file;
		std::string error;
		NsfFile::parse (image.data(), image.size(), file, error);

		check (file.isBanked(), "a program larger than the address space is banked");

		toybox::backends::nsf::NsfBus bus (file);
		bus.load();

		UINT32 value = 0;
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

		INT32 mixed[2] = { 0, 0 };
		apu.Render (mixed);

		INT32 channels[4] = { 0, 0, 0, 0 };
		apu.RenderChannels (channels, 2, 2);

		const int sumLeft = (int) channels[0] + (int) channels[2];
		const int sumRight = (int) channels[1] + (int) channels[3];

		check (std::abs (sumLeft - (int) mixed[0]) <= 2 && std::abs (sumRight - (int) mixed[1]) <= 2,
			   "and their sum is what Render returns");
	}



	std::printf ("\nResult    : %s\n", toybox::test::failures() == 0 ? "all checks passed" : "FAILURES");

	return toybox::test::failures() == 0 ? 0 : 1;
}
