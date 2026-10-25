#pragma once

#include "playback/ChipState.h"
#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include "formats/nsf/NsfFile.h"
#include "xgm/devices/CPU/nes_cpu.h"
#include "xgm/devices/Sound/nes_apu.h"
#include "xgm/devices/Sound/nes_dmc.h"
#include "xgm/devices/Sound/nes_fds.h"
#include "xgm/devices/Sound/nes_fme7.h"
#include "xgm/devices/Sound/nes_mmc5.h"
#include "xgm/devices/Sound/nes_n106.h"
#include "xgm/devices/Sound/nes_vrc6.h"
#include "xgm/devices/Sound/nes_vrc7.h"

namespace toybox::backends::nsf
{

class NsfBus;

/**
	One NSF, running.

	The division of labour is the one NSFPlay settled on, and it is worth stating because it is not
	obvious: the CPU core owns the frame schedule. Exec() runs until the driver's init or play
	returns (it breaks on the JMP-to-self at $4103) and then, when its own frame counter expires,
	re-enters play by itself. A player therefore only has to hand it clocks and tick the chips - it
	must not count frames, or it will call play twice per frame and play nothing at all.

	The DMC's frame sequencer is separate again: Tick() only computes triangle, noise and DPCM, so
	envelopes and length counters advance only when TickFrameSequence() is called alongside it.
*/
class NsfPlayer
{
public:
	NsfPlayer (const formats::nsf::NsfFile& file, double sampleRate);
	~NsfPlayer ();

	NsfPlayer (const NsfPlayer&) = delete;
	NsfPlayer& operator= (const NsfPlayer&) = delete;

	/** Builds the address space, resets the CPU and runs the driver's init. Message thread.

		It also scans the song once, with the output thrown away, to number its DPCM samples. That
		scan is what keeps a sample on one note: numbering them as they are first heard would make
		the note depend on where playback started, and a seek to 0:30 would renumber the kit.
		180 seconds of scan costs about 0.7 s - the whole reason it can be done here at all is
		that the CPU runs some fifteen times faster than the audio it produces.
	*/
	void start (void);

	/** How much of the song the sample scan covers. Past this point a sample that has never been
		heard gets no note rather than a made-up one. */
	static constexpr double scanSeconds = 180.0;

	/** Skips that scan. For a probe that only wants the write counters: the scan costs more than
		everything else about opening a song put together. */
	void skipPrescan (void) { prescanDisabled = true; }

	/** Renders `frames` stereo frames. Returns how many were written. */
	int render (float* left, float* right, int frames);

	/** Renders every channel separately for the same block: dest[i] gets channel i's stereo pair.
		Returns the frames written. Used by renderTracks, and by the check that the sum of the
		channels is what render() produces. */
	int renderChannels (float* const* dest, int channelCount, int frames);

	/** Restarts and runs to `seconds`. An NSF cannot be resumed from the middle - the only
		way to a point is through everything before it. */
	bool seekTo (double seconds);

	/** How many separately audible channels this song has: five on the 2A03 plus whatever
		the declared expansion chip renders. */
	int channelCount (void) const;

	/** Where the 6502 is right now - diagnostics, for files whose driver never writes anything. */
	int currentPC (void) const;

	/** Writes the driver made into the $0200 page - zero means its RAM decoder never ran. */
	std::uint64_t ramWrites (void) const;

	/** CPU clocks executed so far. Against the 1.79 MHz the driver is clocked at, this says
		whether the emulation is being starved or the program itself is just slow. */
	std::uint64_t executedClocks (void) const;

	/** Where the program counter spends its time, one bin per 256-byte page. Sampled inside the
		render loop on purpose: after it returns the CPU is always parked at the trampoline, which is
		why every file looked identical there. */
	const std::array<std::uint32_t, 256>& pcHistogram (void) const { return pcPages; }

	/** How many times the $0211 gate was written, and with what - 0 means init never reached it. */
	std::uint64_t gateWrites (void) const;
	int gateValue (void) const;

	/** Solos one channel of that list; -1 leaves them all audible. */
	void setTrackAudible (int trackIndex);

	/** Mutes every channel. A player built for one track needs this for the case where that
		track is not audible at all. */
	void setSilent (bool silent);

	/** Silences a whole chip. 0 is the 2A03 (both of its objects), 1 the expansion. */
	void setChipAudible (int device, bool audible);

	/** Fills what the chips can report: N163 for now, because it is the only chip in this core
		that exposes per-channel state including the waveform it is reading. */
	bool captureState (playback::ChipStateSnapshot& snapshot) const;

	/** Scales the output. The NES mix table leaves a lot of headroom - five four-bit channels never
		approach full scale - so a file played back unamplified sits eight to twelve decibels below a
		VGM of the same music. The default is the +8 dB the settings page offers. */
	void setOutputGain (float linear);

	/** True when the file declared an expansion chip and one was created. */
	bool hasExpansion (void) const;

	/** Writes seen in each expansion range (0 VRC6, 1 VRC7, 2 N163, 3 FME7), counted even
		when no such chip was created - the evidence a file uses one it did not declare. */
	std::uint32_t expansionWrites (int index) const;

	/** Writes to that chip's own registers rather than to its address range - the evidence a probe
		should believe, because a driver that merely uses the range as memory writes other addresses. */
	std::uint32_t expansionSignature (int index) const;

	std::uint64_t soundWrites (void) const;
	double positionSeconds (void) const;

private:
	void resetToStart (void);
	void scanSampleChannels (double seconds);
	void observeDpcmSample (void);

	/** The MIDI note a DPCM sample slot was given, or -1 when the scan never met it.

		Keyed by the sample's address and its playback rate, because those two together are what
		sets the pitch: the same bytes at half the rate are an octave down, and FamiStudio gives
		them a note of their own for exactly that reason.
	*/
	int noteForDpcmSample (int tone, int rate) const;

	void writeSound (std::uint16_t address, std::uint8_t value);
	void applyMasks (void);
	bool chipAudible[2] = { true, true };

	int soloTrack = -1;
	bool silent = false;
	float outputGain = 2.5119f;			// +8 dB

	const formats::nsf::NsfFile& file;
	double rate = 44100.0;
	std::uint64_t clocksExecuted = 0;
	std::array<std::uint32_t, 256> pcPages {};
	double cpuClockRest = 0.0;
	std::uint64_t rendered = 0;

	std::unique_ptr<NsfBus> bus;
	std::unique_ptr<xgm::NES_CPU> cpu;
	std::unique_ptr<xgm::NES_APU> apu;
	std::unique_ptr<xgm::NES_DMC> dmc;
	std::unique_ptr<xgm::NES_VRC6> vrc6;
	std::unique_ptr<xgm::NES_VRC7> vrc7;
	std::unique_ptr<xgm::NES_N106> n106;
	std::unique_ptr<xgm::NES_FDS> fds;
	std::unique_ptr<xgm::NES_FME7> fme7;
	std::unique_ptr<xgm::NES_MMC5> mmc5;

	/** The DPCM scan. `sampleNotes` is built by one pass over the song and then only read, so the
		note a sample has cannot change while the song plays. */
	std::unordered_map<int, int> sampleNotes;
	int nextSampleNote = 60;			// MIDI 60: the octave FamiStudio starts allocating from
	bool prescanDisabled = false;
	bool scanning = false;
	bool sampleDirty = false;			// a whole slot has been written since the last look
	bool sampleRateWritten = false;
	bool sampleAddrWritten = false;
};

} // namespace toybox::backends::nsf
