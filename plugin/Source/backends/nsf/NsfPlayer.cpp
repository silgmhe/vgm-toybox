#include "NsfPlayer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "third_party/nsfplay/xgm/devices/CPU/nes_cpu.h"
#include "third_party/nsfplay/xgm/devices/Memory/nes_mem.h"
#include "third_party/nsfplay/xgm/devices/Sound/nes_apu.h"
#include "third_party/nsfplay/xgm/devices/Sound/nes_dmc.h"
#include "third_party/nsfplay/xgm/devices/Sound/nes_n106.h"
#include "third_party/nsfplay/xgm/devices/Sound/nes_vrc6.h"
#include "third_party/nsfplay/xgm/devices/Sound/nes_vrc7.h"

namespace toybox::backends::nsf
{

namespace
{

// The CPU clocks of the two machines. The header's PAL flag decides which, and the chip cores get
// the same number - a PAL song played at the NTSC clock is audibly sharp, not subtly off.
constexpr double kNtscClock = 1789773.0;
constexpr double kPalClock = 1662607.0;

} // namespace

/**
	The CPU's view of the world.

	It is a NES_MEM rather than a memory class of our own because the CPU core needs one: its
	`run_from` writes a three-byte trampoline into the reserved area at $4100 and jumps through it,
	and a plain IDevice leaves that pointer null. Deriving from it also keeps the reset state, the
	RAM mirroring and the read path exactly as the core expects, so only two things are ours: which
	writes leave the memory map (the sound registers and the expansion chips), and nothing else.
*/
class NsfPlayer::Bus final : public xgm::NES_MEM
{
public:
	explicit Bus (NsfPlayer& ownerPlayer) : owner (ownerPlayer) {}

	/**
		A banked program is not in the image the base class holds: the player keeps the whole file
		and the banks decide which 8 KB window of it the CPU sees. Reads therefore have to come from
		the file, not from the 64 KB image, or every switch would read the same bytes.
	*/
	bool Read (UINT32 address, UINT32& value, UINT32 id) override
	{
		const auto adr = (std::uint16_t) address;

		if (owner.banked && adr >= 0x8000)
		{
			const int window = (adr >> 13) & 0x03;		// $8000, $A000, $C000, $E000
			const std::uint32_t offset = ((std::uint32_t) owner.banks[window] << 13) + (adr & 0x1FFF);

			value = offset < owner.file.rom.size() ? owner.file.rom[offset] : 0;
			return true;
		}

		return xgm::NES_MEM::Read (address, value, id);
	}

	bool Write (UINT32 address, UINT32 value, UINT32 id) override
	{
		const auto adr = (std::uint16_t) address;

		if (adr >= 0x4000 && adr <= 0x4017)
		{
			owner.writeSoundRegister (adr, (std::uint8_t) value);
			return true;
		}

		if (owner.writeExpansion (adr, (std::uint8_t) value))
		{
			return true;
		}

		// The eight registers at $5FF8 select which bank each window shows. The file has to be
		// larger than the address space for them to mean anything, which is what `banked` says.
		if (owner.banked && adr >= 0x5FF8 && adr <= 0x5FFB)
		{
			owner.banks[adr - 0x5FF8] = (std::uint8_t) value;
			return true;
		}

		return xgm::NES_MEM::Write (address, value, id);
	}

private:
	NsfPlayer& owner;
};

NsfPlayer::NsfPlayer (double sampleRate)
	: rate (sampleRate > 0.0 ? sampleRate : 44100.0)
{
}

NsfPlayer::~NsfPlayer() = default;

bool NsfPlayer::load (const void* data, std::size_t size, std::string& error)
{
	if (! toybox::formats::nsf::parseNsf (data, size, file, error))
	{
		return false;
	}

	// The PAL flag is the lowest bit of the speed field; the rest of it is the frame length.
	pal = (file.ntscSpeed & 1u) != 0;
	clockHz = pal ? kPalClock : kNtscClock;
	clocksPerSample = clockHz / rate;

	bus = std::make_unique<Bus> (*this);
	bus->Reset();

	banked = file.rom.size() > 0x8000;

	for (int i = 0; i < 4; ++i)
	{
		banks[i] = (std::uint8_t) i;
	}

	if (! bus->SetImage (file.rom.data(), file.loadAddress, (UINT32) file.rom.size()))
	{
		error = "the program does not fit the address space";
		return false;
	}

	// The CPU reaches the init and play entries through a trampoline: run_from() writes the target
	// address into \/\ and jumps to \, expecting a JSR there followed by a loop that
	// the core breaks on. These six bytes have to be placed after SetImage, which clears the whole
	// 64 KB image - writing them first means the CPU fetches zeroes and never reaches the driver.
	static const UINT8 kPlayerProgram[] = { 0x20, 0x00, 0x00, 0x4C, 0x03, 0x41 };
	bus->SetReserved (kPlayerProgram, (UINT32) sizeof (kPlayerProgram));

	cpu = std::make_unique<xgm::NES_CPU> (clockHz);
	cpu->SetMemory (bus.get());
	cpu->SetNESMemory (bus.get());
	// Reset installs the CPU's memory callbacks - without it the first instruction fetch goes
	// through a null pointer. It also leaves the PC at  waiting for Start to point it at init.
	cpu->Reset();

	apu = std::make_unique<xgm::NES_APU>();
	apu->SetClock (clockHz);
	apu->SetRate (rate);
	apu->Reset();

	dmc = std::make_unique<xgm::NES_DMC>();
	dmc->SetClock (clockHz);
	dmc->SetRate (rate);
	dmc->SetPal (pal);
	dmc->SetAPU (apu.get());
	dmc->SetCPU (cpu.get());
	dmc->SetMemory (bus.get());
	dmc->Reset();

	if (file.chip == toybox::formats::nsf::NsfChip::vrc6)
	{
		vrc6 = std::make_unique<xgm::NES_VRC6>();
		vrc6->SetClock (clockHz);
		vrc6->SetRate (rate);
		vrc6->Reset();
	}
	else if (file.chip == toybox::formats::nsf::NsfChip::vrc7)
	{
		vrc7 = std::make_unique<xgm::NES_VRC7>();
		vrc7->SetClock (clockHz);
		vrc7->SetRate (rate);
		vrc7->Reset();
	}
	else if (file.chip == toybox::formats::nsf::NsfChip::n163)
	{
		n163 = std::make_unique<xgm::NES_N106>();
		n163->SetClock (clockHz);
		n163->SetRate (rate);
		n163->Reset();
	}

	// The CPU core owns the init/play schedule: it is given both addresses and the frame rate, and
	// runs init here (inside Start) and play from then on. Region: 0 for NTSC, 1 for PAL.
	cpu->Start ((int) file.initAddress, (int) file.playAddress,
				toybox::formats::nsf::framesPerSecond (file),
				file.startSong > 0 ? file.startSong - 1 : 0, pal ? 1 : 0, 0, false, nullptr);


	accumulator = 0.0;
	seconds = 0.0;
	running = true;
	finished = false;
	return true;
}

bool NsfPlayer::writeExpansion (std::uint16_t address, std::uint8_t value)
{
	// The expansion chips live in the program's address space, so they are checked before the
	// memory map - which would otherwise read them as ROM and swallow the write.
	if (vrc6 != nullptr && address >= 0x9000 && address <= 0xB002)
	{
		vrc6->Write (address, value);
		return true;
	}

	if (vrc7 != nullptr && (address == 0x9010 || address == 0x9030))
	{
		vrc7->Write (address, value);
		return true;
	}

	if (n163 != nullptr && (address == 0x4800 || address == 0xF800))
	{
		n163->Write (address, value);
		return true;
	}

	return false;
}

void NsfPlayer::writeSoundRegister (std::uint16_t address, std::uint8_t value)
{
	++writes;
	// The 2A03 is split in two cores: the squares answer $4000-$4007, the triangle, noise and DPCM
	// answer from $4008 up, and $4015 enables channels on both.
	if (address <= 0x4007)
	{
		apu->Write (address, value);
		return;
	}

	if (address == 0x4015)
	{
		apu->Write (address, value);
	}

	dmc->Write (address, value);
}

void NsfPlayer::tickChips (std::uint32_t clocks)
{
	apu->Tick (clocks);
	dmc->Tick (clocks);

	// The frame sequencer is what advances the envelopes and the length counters. Tick() does not
	// call it, so without this every note keeps whatever level it was given and the song becomes a
	// drone - audible, correct in pitch, and wrong in every articulation.
	dmc->TickFrameSequence (clocks);

	if (vrc6 != nullptr) { vrc6->Tick (clocks); }
	if (vrc7 != nullptr) { vrc7->Tick (clocks); }
	if (n163 != nullptr) { n163->Tick (clocks); }
}

void NsfPlayer::renderChips (float* left, float* right)
{
	INT32 square[2] = { 0, 0 };
	INT32 tnd[2] = { 0, 0 };

	apu->Render (square);
	dmc->Render (tnd);

	double mixed = (double) (square[0] + tnd[0]) / fullScale;

	INT32 expansion[2] = { 0, 0 };

	if (vrc6 != nullptr) { vrc6->Render (expansion); }
	else if (vrc7 != nullptr) { vrc7->Render (expansion); }
	else if (n163 != nullptr) { n163->Render (expansion); }

	mixed += (double) expansion[0] / fullScale;

	if (left != nullptr)
	{
		*left += (float) mixed;
	}

	if (right != nullptr)
	{
		*right += (float) mixed;
	}
}

void NsfPlayer::render (float* left, float* right, int samples)
{
	if (! running || finished)
	{
		return;
	}

	for (int i = 0; i < samples; ++i)
	{
		accumulator += clocksPerSample;
		const auto clocks = (std::uint32_t) accumulator;
		accumulator -= (double) clocks;

		if (clocks > 0)
		{
			cpu->Exec ((int) clocks);		// runs init once, then play at each frame boundary
			tickChips (clocks);
		}

		renderChips (left + i, right + i);
	}

	seconds += (double) samples / rate;
}

double NsfPlayer::positionSeconds (void) const
{
	return seconds;
}

} // namespace toybox::backends::nsf
