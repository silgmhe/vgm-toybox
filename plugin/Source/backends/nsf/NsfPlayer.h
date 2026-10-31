#pragma once

#include "playback/ChipState.h"
#include "playback/MidiSequence.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
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

/** What one pass over a song produces: the note each DPCM sample slot was given, and every note
	the song plays. Both come out of the same walk over the file, so they travel together. */
struct ScanResult
{
	std::unordered_map<int, int> sampleNotes;
	std::shared_ptr<const playback::MidiSequence> midiSequence;
};

/** One song's scan, shared by the object that runs it and the object that plays it.

	The walk needs a player of its own - it resets the CPU and runs the song to its end - so its
	result has to cross from that player to the one rendering audio. It used to cross through a
	file-level map keyed by content hash, which left the song's own backend watching from outside
	something it had asked for. This is the handle both sides hold instead; the cache is still
	there, but only so that reopening the same file does not walk it again.
*/
class ScanHandle
{
public:
	void publish (std::shared_ptr<const ScanResult> result)
	{
		const std::lock_guard<std::mutex> guard (mutex);
		value = std::move (result);
	}

	std::shared_ptr<const ScanResult> snapshot (void) const
	{
		const std::lock_guard<std::mutex> guard (mutex);
		return value;
	}

private:
	mutable std::mutex mutex;
	std::shared_ptr<const ScanResult> value;
};

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
	uint64_t ramWrites (void) const;

	/** CPU clocks executed so far. Against the 1.79 MHz the driver is clocked at, this says
		whether the emulation is being starved or the program itself is just slow. */
	uint64_t executedClocks (void) const;

	/** Where the program counter spends its time, one bin per 256-byte page. Sampled inside the
		render loop on purpose: after it returns the CPU is always parked at the trampoline, which is
		why every file looked identical there. */
	const std::array<uint32_t, 256>& pcHistogram (void) const { return pcPages; }

	/** How many times the $0211 gate was written, and with what - 0 means init never reached it. */
	uint64_t gateWrites (void) const;
	int gateValue (void) const;

	/** Solos one channel of that list; -1 leaves them all audible. */
	void setTrackAudible (int trackIndex);

	/** Mutes every channel. A player built for one track needs this for the case where that
		track is not audible at all. */
	void setSilent (bool silent);

	/** Silences a whole chip. 0 is the 2A03 (both of its objects), 1 the expansion. */
	void setChipAudible (int device, bool audible);

	/** Fills what the chips can report: the five 2A03 voices and every row of whichever
		expansion chip the file declares.

		`includeWaves` off skips the waveform copies an FDS or N163 row carries. The preset scan
		reads this tens of thousands of times and never draws anything, and those copies are the
		bulk of the cost - 64 floats for the FDS, up to 256 for each of the N163's eight rows. */
	bool captureState (playback::ChipStateSnapshot& snapshot, bool includeWaves = true) const;

	/** Scales the output. The NES mix table leaves a lot of headroom - five four-bit channels never
		approach full scale - so a file played back unamplified sits eight to twelve decibels below a
		VGM of the same music. The default is the +8 dB the settings page offers. */
	void setOutputGain (float linear);

	/** True when the file declared an expansion chip and one was created. */
	bool hasExpansion (void) const;

	/** Writes seen in each expansion range (0 VRC6, 1 VRC7, 2 N163, 3 FME7), counted even
		when no such chip was created - the evidence a file uses one it did not declare. */
	uint32_t expansionWrites (int index) const;

	/** Writes to that chip's own registers rather than to its address range - the evidence a probe
		should believe, because a driver that merely uses the range as memory writes other addresses. */
	uint32_t expansionSignature (int index) const;

	uint64_t soundWrites (void) const;
	double positionSeconds (void) const;

	/** The MIDI stream the preset scan produced, or null when the scan has not run.

		The scan that numbers the DPCM samples already walks the whole song; recording the note
		changes on the way costs almost nothing extra, and it is what the mixer's MIDI column
		needs. Null is the honest answer before start(): nothing has been read yet.

		Shared rather than borrowed because it outlives the player it came from: the backend holds
		on to it, and a player that handed out a raw pointer would leave that pointer dangling the
		moment the player that scanned the file goes away.
	*/
	std::shared_ptr<const playback::MidiSequence> shareMidiSequence (void) const;

	/** The handle this player publishes its scan into, and reads one from.

		Both players in a song are given the same handle by the backend that owns them: the one
		that runs the walk publishes, the one that renders picks it up on its next look. Without
		it each player would search the file-level cache for an answer its own song already had.
	*/
	void setScanHandle (std::shared_ptr<ScanHandle> handle) { scanHandle = std::move (handle); }

	/** Publishes a scan an earlier session already made into `handle`, when there is one.

		Static because the backend asks before it has a player to ask with: a file that has been
		walked once should not start a thread at all the next time it is opened.
	*/
	static bool adoptCachedScan (const formats::nsf::NsfFile& file, ScanHandle& handle);

	/** Runs the whole-song pass that numbers the DPCM samples and records every note.

		Called by the backend on its own thread, not from start(): the walk is 0.3 to 2.8 seconds
		depending on the expansion chip, and a song has to be playable before it finishes. The
		result is published into this player's handle, so the player rendering audio picks it up
		on its next look.
	*/
	void scanSong (void);

	/** Asks the scan to stop early. Checked once per block; a scan that has been asked to stop
		still publishes whatever it had, because partial numbering beats none. */
	void setScanCancelFlag (const std::atomic<bool>* flag) { scanCancelFlag = flag; }

	/** True while this player is playing a song whose scan has not landed yet. */
	bool isAnalysing (void) const { return analysing.load (std::memory_order_relaxed); }

	/** Diagnostics: how many DPCM samples have started since the song began.

		A hit that sounds longer than one sample can be one sample retriggered, and this is what
		tells that apart from a sample that really is longer. Defined in the .cpp because NsfBus is
		only declared here. */
	uint32_t dpcmStartCount (void) const;

	/** Diagnostics: how many times the DMC's DAC was written, which is where a drum's decay is. */
	uint32_t dacWriteCount (void) const;

	/** Diagnostics: the distinct $4012 pages written, copied into `out`, at most `max` of them.

		A sample longer than 241 bytes has to be played in pieces and the pieces are named by
		walking that register; one page repeated means the driver is replaying one piece. */
	uint32_t dpcmPages (uint8_t* out, uint32_t max) const;

private:
	/** Takes a finished scan into this player. Called from start() and, while waiting, from
		render() - the latter on the audio thread, which is why the handle hands back a shared_ptr
		rather than making the caller hold a lock. */
	void applyScan (const ScanResult& scan);

	void resetToStart (void);
	void scanSampleChannels (double seconds);
	void observeDpcmSample (void);

	/** The MIDI note a DPCM sample slot was given, or -1 when the scan never met it.

		Keyed by the sample's address and its playback rate, because those two together are what
		sets the pitch: the same bytes at half the rate are an octave down, and FamiStudio gives
		them a note of their own for exactly that reason.
	*/
	int noteForDpcmSample (int tone, int rate) const;

	void writeSound (uint16_t address, uint8_t value);
	void applyMasks (void);
	bool chipAudible[2] = { true, true };

	int soloTrack = -1;
	bool silent = false;
	float outputGain = 2.5119f;			// +8 dB

	const formats::nsf::NsfFile& file;
	double rate = 44100.0;
	uint64_t clocksExecuted = 0;
	std::array<uint32_t, 256> pcPages {};
	double cpuClockRest = 0.0;
	uint64_t rendered = 0;

	/** The last DPCM sample decoded, and what it was decoded from.

		Cached rather than decoded per frame because captureState() runs on the audio thread: a
		sample is up to 241 bytes of integration, which is not much once, and is waste at fifteen
		frames a second while the same bytes are being re-read. The pair below is the whole key -
		a sample does not change while it plays.
	*/
	mutable std::vector<float> dpcmWave;
	mutable uint16_t dpcmWaveStart = 0;
	mutable uint16_t dpcmWaveLength = 0;
	mutable uint32_t dpcmWaveStarts = 0;
	mutable uint32_t dpcmWavePages = 0;

	/** How many bytes of the copy are this sample: the register's length, less the filler that
		follows the data and the padding byte the formula adds. The arithmetic is in DpcmSample.h. */
	uint16_t dpcmSampleLength (void) const;

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

	/** The other product of that same pass: every note the song plays, in time order. Shared
		because the scan is cached and every player built from this file gets the same stream. */
	std::shared_ptr<const playback::MidiSequence> midiSequence;

	/** Set while the scan for this song has not landed. Cleared by applyScan() the moment the
		song's handle has the result, which is what turns the DPCM row's "analysing" label off.

		Atomic because it is written from start() and read from render(): the same player is driven
		by the message thread when it is set up and by the audio thread when it plays.
	*/
	std::atomic<bool> analysing { false };

	/** The song's scan, shared with the other player in the same song. Null only for a player
		built outside a backend - the tests construct one directly to read registers. */
	std::shared_ptr<ScanHandle> scanHandle;

	/** Read by the scan loop, written by whoever owns the scanning thread's lifetime. */
	const std::atomic<bool>* scanCancelFlag = nullptr;

	bool prescanDisabled = false;

	/** True while the whole-song pass is running on this player. Atomic for the same reason as
		`analysing`: the pass runs on the scanning thread and render() tests it on the audio one. */
	std::atomic<bool> scanning { false };
	bool sampleDirty = false;			// a whole slot has been written since the last look
	bool sampleRateWritten = false;
	bool sampleAddrWritten = false;
};

} // namespace toybox::backends::nsf
