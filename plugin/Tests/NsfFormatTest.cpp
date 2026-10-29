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

		// Only the low four bits of the length reach the chip, and the page register's upper half is
		// not a page at all - a driver that writes a byte with bits set above them still plays a
		// seventeen-byte sample at $C1C0.
		bus.Write (0x4015, 0x00);			// stops it, which is what arms the next one
		bus.Write (0x4012, 0x07);
		bus.Write (0x4013, 0xF1);
		bus.Write (0x4015, 0x10);
		check (bus.dpcm.start == 0xC1C0 && bus.dpcm.length == 17,
			   "and the upper bits of both registers are dropped");

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



	std::printf ("\nResult    : %s\n", toybox::test::failures() == 0 ? "all checks passed" : "FAILURES");

	return toybox::test::failures() == 0 ? 0 : 1;
}
