#include "backends/nsf/NsfPlayer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include "backends/nsf/DpcmSample.h"
#include "backends/nsf/NsfBus.h"
#include "backends/nsf/NsfChannelNaming.h"
#include "playback/WaveShape.h"

namespace toybox::backends::nsf
{

namespace
{

/** NTSC 2A03 cycle rate. A PAL machine runs at 1662607. Both are the CPU's own clock, not a
	multiple: the APU runs from the same crystal divided internally. */
constexpr double kNtscClock = 1789772.727272727;
constexpr double kPalClock = 1662607.0;

/** The shortest note the scan records: one NES frame. A DPCM hit can be over in three
	milliseconds, and a note-off stamped at the same instant as its note-on is a note MIDI
	receivers drop - which would silence exactly the drums a DPCM track is for. */
constexpr double kMinimumNoteSeconds = 1.0 / 60.0;

/** How often a player waiting for its scan looks again, in samples: 4096 is about 93 ms at
	44.1 kHz. Frequent enough that the DPCM row's label clears while the user is still looking
	at it, rare enough that the check costs nothing on the blocks in between. */
constexpr uint64_t kScanPollIntervalSamples = 4096;

/** The rate a DPCM sample is read at, from its own rate index.

	Not from the track info: that answers with whatever the register holds when it is asked, and a
	driver is free to put the register back after a hit. The song this was measured against reported
	the top of the table - 33144 Hz, 46 ms - for a sample it plays at the eighth, and 46 ms is
	exactly why it sounded like a click instead of a kick.

	`cycles` is what the chip counts per bit, so the rate is the CPU clock divided by it.
*/
float dpcmRate (uint8_t index, bool pal)
{
	static constexpr int ntsc[16] = { 428, 380, 340, 320, 286, 254, 226, 214,
									  190, 160, 142, 128, 106, 84, 72, 54 };
	static constexpr int palCycles[16] = { 398, 354, 316, 298, 276, 236, 210, 198,
										   176, 148, 132, 118, 98, 78, 66, 50 };

	const double clock = pal ? kPalClock : kNtscClock;
	const auto cycles = pal ? palCycles[index & 0x0F] : ntsc[index & 0x0F];

	return (float) (clock / (double) cycles);
}

/** MIDI's top velocity. The walk records a channel's level as 0..1, so this is the one place
	where the two scales meet. */
constexpr double kMaxMidiVelocity = 127.0;

/** DPCM sample slots per song, keyed by the file's content hash and its start song.

	A backend is rebuilt several times while one song is open - switching the mixer on, seeking -
	and each rebuild is a fresh player with an empty table. The numbering does not depend on any of
	that, and the scan costs about half a second, so it is kept here rather than on whichever file
	object the current backend happens to own.
*/
std::mutex sampleSlotMutex;
std::unordered_map<std::size_t, ScanResult> sampleSlotCache;

/** Insertion order, so the oldest entry can go when the cache is full. A deque rather than a
	list: only the ends are touched, and it keeps the eviction order in one place instead of
	stamping a counter into every ScanResult. */
std::deque<std::size_t> sampleSlotOrder;

/** Four, not sixteen. A stream is 0.4 to 2 MB depending on the song, so the old limit was a
	sixteen-to-thirty-megabyte ceiling; and when it was reached the whole map was cleared, which
	means listening to one song too many made the one before it scan again from scratch. Four
	songs is what going back and forth actually looks like. */
constexpr std::size_t kSampleSlotCacheLimit = 4;

} // namespace

NsfPlayer::NsfPlayer (const formats::nsf::NsfFile& image, double sampleRate)
	: file (image), rate (sampleRate > 0.0 ? sampleRate : 44100.0)
{
	bus = std::make_unique<NsfBus> (file);
	bus->onSoundWrite = [this] (uint16_t address, uint8_t value)
	{
		writeSound (address, value);
	};

	cpu = std::make_unique<xgm::NES_CPU> (kNtscClock);
	apu = std::make_unique<xgm::NES_APU> ();
	dmc = std::make_unique<xgm::NES_DMC> ();

	// NSFPlay randomises the noise LFSR and the triangle's phase at power-on so that repeated
	// playback does not sound machine-identical. That makes a restart produce different samples -
	// which breaks the one property a mixer depends on: seeking back to the same position has to
	// give the same audio. Off, so a render is a function of position alone.
	dmc->SetOption (xgm::NES_DMC::OPT_RANDOMIZE_NOISE, 0);
	dmc->SetOption (xgm::NES_DMC::OPT_RANDOMIZE_TRI, 0);

	const double clock = file.isPal() ? kPalClock : kNtscClock;

	apu->SetClock (clock);
	apu->SetRate (rate);
	dmc->SetClock (clock);
	dmc->SetRate (rate);
	dmc->SetPal (file.isPal ());

	// The DMC fetches its sample bytes through a memory device; without one the first DPCM read
	// dereferences a null pointer, and a driver is allowed to start one during init.
	// Only the expansion the header declares exists, and each one is fed by the writes its own
	// address range receives - the routing below is the whole of the contract with them.
	const auto chip = file.chip ();

	if (chip == formats::nsf::NsfChip::vrc6)
	{
		vrc6 = std::make_unique<xgm::NES_VRC6> ();
		vrc6->SetClock (clock);
		vrc6->SetRate (rate);
	}

	if (chip == formats::nsf::NsfChip::vrc7)
	{
		vrc7 = std::make_unique<xgm::NES_VRC7> ();
		vrc7->SetClock (clock);
		vrc7->SetRate (rate);
	}

	if (chip == formats::nsf::NsfChip::n163)
	{
		n106 = std::make_unique<xgm::NES_N106> ();
		n106->SetClock (clock);
		n106->SetRate (rate);
	}

	if (chip == formats::nsf::NsfChip::fme7)
	{
		fme7 = std::make_unique<xgm::NES_FME7> ();
		fme7->SetClock (clock);
		fme7->SetRate (rate);
	}

	if (chip == formats::nsf::NsfChip::fds)
	{
		fds = std::make_unique<xgm::NES_FDS> ();
		fds->SetClock (clock);
		fds->SetRate (rate);
	}

	if (chip == formats::nsf::NsfChip::mmc5)
	{
		mmc5 = std::make_unique<xgm::NES_MMC5> ();
		mmc5->SetClock (clock);
		mmc5->SetRate (rate);
	}

	dmc->SetMemory (bus.get ());
	dmc->SetAPU (apu.get ());
	dmc->SetCPU (cpu.get ());
}

NsfPlayer::~NsfPlayer () = default;

void NsfPlayer::start (void)
{
	resetToStart ();

	// The whole-song pass does not happen here any more. It walks 180 seconds of music - 0.3 to
	// 2.8 seconds of CPU depending on the expansion chip - and a file has to be playable before
	// that finishes. The backend runs it on a thread of its own; this only picks up a result the
	// song's handle already has, and says so when it does not.
	if (! prescanDisabled)
	{
		if (auto scan = scanHandle != nullptr ? scanHandle->snapshot () : nullptr)
		{
			applyScan (*scan);
		}
		else
		{
			// A player built outside a backend has no handle, and nothing to wait for.
			analysing.store (scanHandle != nullptr, std::memory_order_relaxed);
		}
	}

	resetToStart ();
}

void NsfPlayer::applyScan (const ScanResult& scan)
{
	sampleNotes = scan.sampleNotes;
	midiSequence = scan.midiSequence;
	analysing.store (false, std::memory_order_relaxed);
}

bool NsfPlayer::adoptCachedScan (const formats::nsf::NsfFile& file, ScanHandle& handle)
{
	const std::size_t key = file.contentHash ^ (std::size_t) file.startSong;

	std::lock_guard<std::mutex> guard (sampleSlotMutex);
	const auto found = sampleSlotCache.find (key);

	if (found == sampleSlotCache.end ())
	{
		return false;
	}

	handle.publish (std::make_shared<const ScanResult> (found->second));
	return true;
}

void NsfPlayer::scanSong (void)
{
	// Another backend may have walked this file already. Reopening a file is common, and the
	// answer does not depend on who asks, so a second walk is spent time for the same result.
	if (scanHandle != nullptr && adoptCachedScan (file, *scanHandle))
	{
		return;
	}

	resetToStart ();
	scanSampleChannels (scanSeconds);
}

uint32_t NsfPlayer::dpcmStartCount (void) const
{
	return bus != nullptr ? bus->dpcmStarts : 0;
}

uint32_t NsfPlayer::dacWriteCount (void) const
{
	return bus != nullptr ? bus->dacWrites : 0;
}

uint16_t NsfPlayer::dpcmSampleLength (void) const
{
	if (bus == nullptr)
	{
		return 0;
	}

	// $4013 as the core reads it, capped at the copy. The cap is arithmetic hygiene rather than a
	// real bound: eight bits of length are 4081 bytes and the window is 4096, so no register that
	// survived its write can reach it.
	const auto length = (std::min) ((std::size_t) bus->dpcm.length, bus->dpcm.bytes.size ());

	// Filler and the padding byte live in DpcmSample.h, where a test can call them.
	return (uint16_t) dpcm::finalLength (length, bus->dpcm.bytes.data ());
}

uint32_t NsfPlayer::dpcmPages (uint8_t* out, uint32_t max) const
{
	if (bus == nullptr || out == nullptr)
	{
		return 0;
	}

	const auto count = (std::min) (max, bus->pageCount);

	for (uint32_t i = 0; i < count; ++i)
	{
		out[i] = bus->pagesSeen[i];
	}

	return count;
}

void NsfPlayer::resetToStart (void)
{
	rendered = 0;
	cpuClockRest = 0.0;
	clocksExecuted = 0;
	pcPages.fill (0);
	sampleRateWritten = false;
	sampleAddrWritten = false;

	// The cache describes bytes in the image that is about to be replaced. Both fields go to values
	// no real selection can hold - a sample never starts below $C000 and is never empty - so the
	// first capture after a restart decodes instead of matching the previous song's drum.
	dpcmWave.clear ();
	dpcmWaveStart = 0;
	dpcmWaveLength = 0;
	dpcmWaveStarts = 0;

	// Order matters, and each step was paid for once already:
	//   1. the image, then the trampoline (SetImage clears the 64 KB it lives in);
	//   2. the bus before the CPU, because Reset() installs the callbacks that read through it;
	//   3. Start() last, which is what actually runs the driver's init.
	bus->load ();

	// NSFPlay resets every attached device through its stack before starting; we own the two
	// devices directly, so they are reset here. Without this the frame sequencer has no phase and
	// TickFrameSequence() has nothing to advance against.
	apu->Reset ();
	dmc->Reset ();

	if (vrc6) { vrc6->Reset (); }
	if (vrc7) { vrc7->Reset (); }
	if (n106) { n106->Reset (); }
	if (fme7) { fme7->Reset (); }

	cpu->SetMemory (bus.get ());
	cpu->SetNESMemory (bus.get ());
	cpu->nes_basecycles = file.isPal () ? kPalClock : kNtscClock;
	cpu->Reset ();

	const int song = file.startSong > 0 ? file.startSong - 1 : 0;
	const int region = file.isPal () ? 1 : 0;

	applyMasks ();

	cpu->Start ((int) file.initAddress, (int) file.playAddress, file.framesPerSecond (), song,
				region, 0, false, nullptr);

}

void NsfPlayer::scanSampleChannels (double seconds)
{
	if (dmc == nullptr || seconds <= 0.0)
	{
		return;
	}

	// 256 samples is 5.8 ms: the notes this scan records land within a sixth of a NES frame of
	// where the driver played them. 1024 was enough for the sample slots, which change once per
	// bar, but a drum hit placed 23 ms late is audible.
	const int block = 256;
	const int total = (int) (seconds * rate);

	std::vector<float> left ((std::size_t) block, 0.0f);
	std::vector<float> right ((std::size_t) block, 0.0f);

	scanning.store (true, std::memory_order_relaxed);
	sampleDirty = false;		// nothing is a sample until the driver has written $4010 or $4012

	// The note each row was last seen playing, and when that note started. A drum can begin and
	// end inside one block, and a note-off stamped at the same instant as its note-on is a note
	// many receivers drop.
	const std::size_t rows = (std::size_t) (std::max) (1, channelCount ());
	std::vector<int> lastNote (rows, -1);
	std::vector<double> noteOnTime (rows, 0.0);
	std::vector<playback::MidiEvent> events;

	for (int done = 0; done < total; done += block)
	{
		// Stopping early leaves the numbering with however far it got, which is still better than
		// none - and it is what closing the song or opening another one asks for.
		if (scanCancelFlag != nullptr && scanCancelFlag->load ())
		{
			break;
		}

		// The output is thrown away. Rendering is the only way to advance the CPU by the schedule
		// the player already keeps, and it is cheap: the emulation runs about fifteen times
		// faster than the audio it produces.
		render (left.data (), right.data (), block);

		// The same reading the keyboard gets, minus the waveforms: this runs thirty thousand
		// times and nothing here draws.
		playback::ChipStateSnapshot snapshot;
		captureState (snapshot, false);

		const double time = (double) (done + block) / rate;

		for (const auto& channel : snapshot.channels)
		{
			const int index = channel.trackIndex;

			if (index < 0 || (std::size_t) index >= rows)
			{
				continue;
			}

			const std::size_t row = (std::size_t) index;
			const int note = (channel.active && channel.note >= 0) ? channel.note : -1;

			if (note == lastNote[row])
			{
				continue;
			}

			if (lastNote[row] >= 0)
			{
				playback::MidiEvent off;
				off.timeSeconds = (std::max) (time, noteOnTime[row] + kMinimumNoteSeconds);
				off.type = playback::MidiEventType::noteOff;
				off.data1 = (uint8_t) lastNote[row];
				off.sourceTrack = (uint32_t) index;
				events.push_back (off);
			}

			if (note >= 0)
			{
				playback::MidiEvent on;
				on.timeSeconds = time;
				on.type = playback::MidiEventType::noteOn;
				on.data1 = (uint8_t) note;
				on.data2 = (uint8_t) (std::clamp) ((int) std::lround ((double) channel.volume * kMaxMidiVelocity),
														1, 127);
				on.sourceTrack = (uint32_t) index;
				events.push_back (on);

				noteOnTime[row] = time;
			}

			lastNote[row] = note;
		}
	}

	scanning.store (false, std::memory_order_relaxed);

	// The minimum-length rule can push a note-off past events recorded after it, and the engine
	// walks this list with lower_bound - time order is the one thing it requires.
	std::stable_sort (events.begin (), events.end (),
					  [] (const playback::MidiEvent& a, const playback::MidiEvent& b)
					  {
						  return a.timeSeconds < b.timeSeconds;
					  });

	auto sequence = std::make_shared<playback::MidiSequence> ();
	sequence->events = std::move (events);

	// No loop point: the NSF header has a field for one and almost nothing fills it, so guessing
	// would put the loop in the wrong bar. -1 leaves it to the length the user configured.
	sequence->loopStartSeconds = -1.0;
	sequence->loopEndSeconds = -1.0;

	midiSequence = std::move (sequence);

	// Published twice, to two different audiences. The handle is how this song's other player
	// hears about it - it asked on its own clock and will not ask again; the cache is how the
	// next session avoids walking the same file a second time.
	auto result = std::make_shared<const ScanResult> (ScanResult { sampleNotes, midiSequence });

	if (scanHandle != nullptr)
	{
		scanHandle->publish (result);
	}

	const std::size_t key = file.contentHash ^ (std::size_t) file.startSong;

	std::lock_guard<std::mutex> guard (sampleSlotMutex);

	// Oldest out, one at a time. The map used to be emptied when it filled, so the song the user
	// had just left was thrown away along with the ones that had gone stale.
	while (sampleSlotOrder.size () >= kSampleSlotCacheLimit)
	{
		sampleSlotCache.erase (sampleSlotOrder.front ());
		sampleSlotOrder.pop_front ();
	}

	sampleSlotOrder.push_back (key);
	sampleSlotCache[key] = *result;
}

std::shared_ptr<const playback::MidiSequence> NsfPlayer::shareMidiSequence (void) const
{
	return midiSequence;
}

void NsfPlayer::observeDpcmSample (void)
{
	auto* info = dmc->GetTrackInfo (2);

	if (info == nullptr)
	{
		return;
	}

	// Deliberately not checking key status: a short sample is finished by the time the scan looks,
	// and the slot would never be recorded. Writing $4012 is the evidence that a slot is about to
	// play, and it is the write that brought us here.
	const int tone = (int) info->GetTone ();
	const int rate = (int) (info->GetFreq () & 0x0F);
	const int key = (tone << 4) | rate;

	if (sampleNotes.find (key) != sampleNotes.end ())
	{
		return;
	}

	// MIDI stops at 127. A song with more slots than that keeps the ones it has rather than
	// wrapping around and giving two samples one note.
	if (nextSampleNote > 126)
	{
		return;
	}

	sampleNotes[key] = nextSampleNote++;
}

int NsfPlayer::noteForDpcmSample (int tone, int rate) const
{
	const int key = (tone << 4) | (rate & 0x0F);
	const auto found = sampleNotes.find (key);

	return found == sampleNotes.end () ? -1 : found->second;
}

void NsfPlayer::writeSound (uint16_t address, uint8_t value)
{
	// $4010 is the sample rate and $4012 the sample address: together they decide which slot is
	// about to play. Neither alone is one, and a driver may write them on different frames - the
	// check has to wait for the second, or the first read records the address at a rate of zero
	// and takes the note the real slot should have had.
	if (address == 0x4010) { sampleRateWritten = true; }
	if (address == 0x4012) { sampleAddrWritten = true; }

	if (sampleRateWritten && sampleAddrWritten)
	{
		sampleDirty = true;
	}

	// $4000-$4013 are the pulse, triangle and noise registers, $4015 the status/enable mask.
	// \-\ are the APU\'s triangle and \-\ belong to the DMC object - that is
	// where its noise and DPCM live. Sending everything to the APU left the last two channels of
	// every song silent while the triangle worked.
	if (address <= 0x4007 || address == 0x4015)
	{
		apu->Write (address, value);
		if (address == 0x4015) { dmc->Write (address, value); }
		return;
	}

	if (address >= 0x4008 && address <= 0x4013)
	{
		dmc->Write (address, value);
		return;
	}

	if (address == 0x4011)
	{
		dmc->Write (address, value);
		return;
	}

	if (vrc6 && address >= 0x9000 && address <= 0xB003)
	{
		vrc6->Write (address, value);
		return;
	}

	if (vrc7 && (address == 0x9010 || address == 0x9030))
	{
		vrc7->Write (address, value);
		return;
	}

	if (n106 && (address == 0x4800 || address == 0xF800))
	{
		n106->Write (address, value);
		return;
	}

	if (fme7 && (address == 0xC000 || address == 0xE000))
	{
		fme7->Write (address, value);
		return;
	}

	// The MMC5's audio lives in the CPU's own address space, $5000-$5015.
	if (mmc5 && address >= 0x5000 && address <= 0x5015)
	{
		mmc5->Write (address, value);
		return;
	}

	// The FDS answers $4023 (its master I/O switch) and its register file at $4040-$408A,
	// which includes the 64-byte wavetable at $4040-$407F.
	if (fds && (address == 0x4023 || (address >= 0x4040 && address <= 0x408A)))
	{
		fds->Write (address, value);
		return;
	}

	if (address == 0x4017)
	{
		// The frame counter register is shared: the APU sequences on it and the DMC's IRQ does too.
		apu->Write (address, value);
		dmc->Write (address, value);
	}
}

bool NsfPlayer::hasExpansion (void) const
{
	return vrc6 != nullptr || vrc7 != nullptr || n106 != nullptr || fme7 != nullptr
		   || fds != nullptr || mmc5 != nullptr;
}

int NsfPlayer::render (float* left, float* right, int frames)
{
	if (frames <= 0)
	{
		return 0;
	}

	// The scan is published by a thread that tells nobody, so this looks again on its own clock -
	// every 4096 samples, about 93 ms. Without it the DPCM row would say "analysing" until the
	// song was reopened, because the only other place that checks runs from start().
	// Not while scanning: that pass is trying to produce the very thing being waited for, and
	// looking from inside it would only take the handle's lock between its own writes.
	// "Crossed a poll boundary during this block" rather than a modulo on where the block starts:
	// the same condition, said so that a reader does not have to convince themselves the interval
	// is larger than a block for it to work.
	const auto pollBoundary = rendered / kScanPollIntervalSamples;
	const auto nextBoundary = (rendered + (uint64_t) frames) / kScanPollIntervalSamples;

	if (analysing.load (std::memory_order_relaxed) && ! scanning.load (std::memory_order_relaxed)
		&& pollBoundary != nextBoundary)
	{
		if (auto scan = scanHandle != nullptr ? scanHandle->snapshot () : nullptr)
		{
			applyScan (*scan);
		}
	}


	const double clocksPerSample = (file.isPal () ? kPalClock : kNtscClock) / rate;

	for (int i = 0; i < frames; ++i)
	{
		cpuClockRest += clocksPerSample;


		const auto clocks = (int) cpuClockRest;

		if (clocks > 0)
		{
			const int executed = cpu->Exec (clocks);
		clocksExecuted += (uint64_t) (std::max) (0, executed);

		// One sample every 64 frames is enough to see where the time goes and cheap enough to
		// leave the emulation at full speed.
		if ((i & 63) == 0)
		{
			++pcPages[(cpu->GetPC () >> 8) & 0xFF];
		}
			cpuClockRest -= (double) executed;

			// Envelopes and length counters live here; Tick() alone leaves every note at the level
			// it was given.
			dmc->TickFrameSequence ((uint32_t) (std::max) (0, executed));

			const auto clocks = (uint32_t) (std::max) (0, executed);
			apu->Tick (clocks);
			dmc->Tick (clocks);
			if (vrc6) { vrc6->Tick (clocks); }
			if (vrc7) { vrc7->Tick (clocks); }
			if (n106) { n106->Tick (clocks); }
			if (fme7) { fme7->Tick (clocks); }
			if (fds) { fds->Tick (clocks); }
			if (mmc5) { mmc5->Tick (clocks); }

		}

		if (scanning && sampleDirty)
		{
			observeDpcmSample ();
			sampleDirty = false;
		}

		int32_t a[2] = { 0, 0 };
		int32_t d[2] = { 0, 0 };
		apu->Render (a);
		dmc->Render (d);

		int32_t extra[2] = { 0, 0 };

		const auto addChip = [&extra] (xgm::ISoundChip* chip)
		{
			if (chip == nullptr)
			{
				return;
			}

			int32_t out[2] = { 0, 0 };
			chip->Render (out);
			extra[0] += out[0];
			extra[1] += out[1];
		};

		addChip (vrc6.get ());
		addChip (vrc7.get ());
		addChip (n106.get ());
		addChip (fme7.get ());
		addChip (fds.get ());
		addChip (mmc5.get ());

		const auto mix = [&] (int channel, float& out)
		{
			const int value = (int) a[channel] + (int) d[channel] + (int) extra[channel];
			out = (std::clamp) ((float) value / 32768.0f * outputGain, -1.0f, 1.0f);
		};

		mix (0, left[i]);
		mix (1, right[i]);

	}

	rendered += (uint64_t) frames;
	return frames;
}

uint32_t NsfPlayer::expansionWrites (int index) const
{
	if (bus == nullptr || index < 0 || index >= 4)
	{
		return 0;
	}

	return bus->expansionWrites[(std::size_t) index];
}

uint32_t NsfPlayer::expansionSignature (int index) const
{
	if (bus == nullptr || index < 0 || index >= 4)
	{
		return 0;
	}

	return bus->expansionSignature[(std::size_t) index];
}

uint64_t NsfPlayer::soundWrites (void) const
{
	return bus != nullptr ? bus->soundWriteCount : 0;
}

double NsfPlayer::positionSeconds (void) const
{
	return (double) rendered / rate;
}

bool NsfPlayer::seekTo (double seconds)
{
	if (seconds < 0.0)
	{
		return false;
	}

	// Restart, then run the driver until the position is reached. There is no state to seek into:
	// the music lives in a program that has to be executed from its beginning.
	start ();

	// Big steps, not one sample at a time. The CPU keeps its own frame schedule, so the size of the
	// step does not change when play is called - it only decides how often this loop looks at the
	// clock. Forty clocks per step would be one sample's worth; four thousand is a hundred times
	// less bookkeeping for exactly the same emulation.
	constexpr int clocksPerStep = 4096;

	// The target is in CPU clocks, not in samples: rendered counts samples everywhere else, and
	// adding clocks to it made every seek land about forty times short of where it was asked to
	// go - the position said 90 s while the audio was at 2.2 s, and each instance stopped at a
	// slightly different one of those wrong places, which is the spread the mixer measured.
	const double clock = file.isPal () ? kPalClock : kNtscClock;
	const auto targetClocks = (uint64_t) (seconds * clock);

	uint64_t clocksRun = 0;

	while (clocksRun < targetClocks)
	{
		// The last step is clipped to what remains, so the run lands on the target rather than
		// past it: an overshoot of one step is 2.3 ms, and thirteen instances overshooting by
		// different amounts is exactly the drift the mixer reported between tracks.
		const auto remaining = targetClocks - clocksRun;
		const int step = (int) (remaining < (uint64_t) clocksPerStep ? remaining
																		 : (uint64_t) clocksPerStep);
		const int executed = cpu->Exec (step);
		const auto clocks = (uint32_t) (std::max) (0, executed);

		dmc->TickFrameSequence (clocks);
		apu->Tick (clocks);
		dmc->Tick (clocks);
		if (vrc6) { vrc6->Tick (clocks); }
		if (vrc7) { vrc7->Tick (clocks); }
		if (n106) { n106->Tick (clocks); }
		if (fme7) { fme7->Tick (clocks); }
		if (fds) { fds->Tick (clocks); }
		if (mmc5) { mmc5->Tick (clocks); }


		clocksRun += (uint64_t) (std::max) (0, executed);
	}

	rendered = (uint64_t) ((double) clocksRun / clock * rate);

	return true;
}

void NsfPlayer::setTrackAudible (int trackIndex)
{
	soloTrack = trackIndex;
	applyMasks ();
}

namespace
{
	// What the snapshot calls things. The strings appeared twice in the capture (the chip list and
	// each channel), and a typo in one of them produced a channel that named a chip the snapshot
	// did not declare - the exact shape the conformance check now rejects.
	constexpr const char* kChipName2A03 = "2A03";
	constexpr const char* kChipNameN163 = "N163";

	constexpr int kPulseChannels = 2;
	constexpr int kDmcChannels = 3;				// triangle, noise and the delta channel
	// The 2A03's five tracks come first in the snapshot's channel order.
	constexpr int kFirstDmcTrack = kPulseChannels;
	// Six voices on the VRC7's OPLL. The chip addresses nine, but a NES cartridge wires six.
	constexpr int kVrc7Channels = 6;

	// Every expansion chip starts its tracks here; a file declares at most one of them.
	constexpr int kFirstExpansionTrack = kPulseChannels + kDmcChannels;
	constexpr int kFirstN163Track = kFirstExpansionTrack;

	// $4000 and $4004: duty/sweep, then timer low, timer high - four registers apart.
	constexpr int kPulseRegisterStride = 4;
	constexpr int kDutyShift = 6;
	constexpr int kDutyMask = 0x03;
	constexpr float kDutySettings[] = { 0.125f, 0.25f, 0.5f, 0.75f };

	/** The VRC6's eight widths, in sixteenths: 1/16 up to 8/16. The widest is a square wave and
		not silence, which is why these are not eighths - and why dividing by the count of settings
		drew a 50 % pulse as a solid bar. */
	constexpr float kVrc6DutySettings[] = { 0.0625f, 0.125f, 0.1875f, 0.25f,
											0.3125f, 0.375f, 0.4375f, 0.5f };

	/** The shapes a channel can have, as playback/ChipState.h numbers them. */
	enum WaveShape
	{
		kShapeStored = 1,		// a real table, read out of the chip
		kShapePulse = 2,
		kShapeTriangle = 3,
		kShapeNoise = 4,
		kShapeDelta = 5,
		kShapeSaw = 6,			// the VRC6's ramp: a stepped line, not a stored table
		kShapeFm = 7			// the VRC7's FM - a pitch and a level, no waveform to read
	};

	// The 2A03's mixer in this core is four bits wide per output.
	constexpr int kMixerMaximum = 15;
	constexpr float kMixerMaximumFloat = 15.0f;

	// The N163's wave RAM holds at most 256 four-bit samples per channel.
	// A channel with no duty cycle of its own - noise, the delta channel, the triangle - still has
	// to fill the field, and half is what the panel draws as a centred line.
	constexpr float kNoDuty = 0.5f;

	// Where the noise channel's sixteen period settings land in MIDI. FamiStudio lays them out as
	// sixteen semitones starting at its note 32, in a scheme whose C4 is 49; MIDI's C4 is 60, so
	// the same arrangement starts eleven higher. Period 0 is the brightest, hence the reversal.
	constexpr int kNoiseNoteFloor = 43;

	// A chip that reports no maximum volume would divide by zero; one is the smallest sensible.
	constexpr int kLowestMaximumVolume = 1;

	constexpr int kN163MaximumWaveLength = 256;
	constexpr float kN163SampleMaximum = 15.0f;
}

bool NsfPlayer::captureState (playback::ChipStateSnapshot& snapshot, bool includeWaves) const
{
	snapshot.chips.clear ();
	snapshot.channels.clear ();
	snapshot.positionSeconds = positionSeconds ();
	snapshot.valid = false;

	// Always the 2A03; the expansion the header declares on top of it.
	snapshot.chips.push_back (kChipName2A03);

	switch (file.chip ())
	{
	case formats::nsf::NsfChip::vrc6: snapshot.chips.push_back ("VRC6"); break;
	case formats::nsf::NsfChip::vrc7: snapshot.chips.push_back ("VRC7"); break;
	case formats::nsf::NsfChip::fds:  snapshot.chips.push_back ("FDS");  break;
	case formats::nsf::NsfChip::mmc5: snapshot.chips.push_back ("MMC5"); break;
	case formats::nsf::NsfChip::n163: snapshot.chips.push_back (kChipNameN163); break;
	case formats::nsf::NsfChip::fme7: snapshot.chips.push_back ("S5B"); break;
	case formats::nsf::NsfChip::none: break;
	}

	// The chip list is what a panel shows when no channel can be described; the channels below
	// cover every chip that reports them.
	snapshot.valid = true;

	const auto addChannel = [&snapshot, this] (const char* name, const char* chipName, int trackIndex,
										 int waveKind, float duty, xgm::ITrackInfo* info) -> std::size_t
	{
		playback::ChannelState state;
		state.name = name;
		state.chipName = chipName;
		state.trackIndex = trackIndex;
		state.waveKind = waveKind;
		state.duty = duty;

		if (info != nullptr)
		{
			state.frequency = (float) info->GetFreqHz ();
			state.active = info->GetKeyStatus ();

			// A DPCM sample lasts a few milliseconds, and a block boundary usually falls between
			// two of them, so key status is false far more often than true - the row would blink
			// out between hits and the note would never be seen. A DAC that still holds a value is
			// the evidence that this voice has been used at all. GetOutput() rather than GetVolume(): the
			// volume field is the register the driver wrote once, and this core never updates it as
			// the sample plays.
			if (waveKind == kShapeDelta && ! state.active && info->GetOutput () > 0)
			{
				state.active = true;
			}

			// Two of the 2A03's five voices have no pitch of their own and both answer with a note
			// anyway, because a panel has nothing to draw and a MIDI port has nothing to send
			// without one. What the note means differs:
			//
			//   the noise channel's sixteen period settings are laid out as sixteen semitones -
			//   FamiStudio's arrangement - so a hi-hat written at one period is never the kick
			//   written at another. It is a numbering, not a frequency: the real periods span ten
			//   octaves, and none of that survives an instrument.
			//
			//   the DPCM channel plays whatever sample it is pointed at, so its note is the slot
			//   the scan gave that sample. Same bytes at the same rate, same note - which is what
			//   lets a drum kit built against one song be reused in it.
			if (waveKind == kShapeNoise)
			{
				// The period is both the note and the shape: it is what makes the sixteen settings
				// sound different, and the panel has nothing else to draw it from.
				state.note = ((int) info->GetFreq () ^ 0x0F) + kNoiseNoteFloor;
				state.shapeParameter = (int) info->GetFreq ();
			}
			else if (waveKind == kShapeDelta)
			{
				const int tone = (int) info->GetTone ();
				const int rate = (int) info->GetFreq ();

				state.note = noteForDpcmSample (tone, rate);

				// The slot this sample would be is not known until the walk finishes, so the row
				// says so rather than showing a note that would change under it. Everything else
				// about the row - the shape, the level, whether it is sounding - is known now.
				state.analysing = analysing.load (std::memory_order_relaxed);

				// The slot's identity, spread so that address and rate each move the drawing -
				// a sample played at two speeds is two marks, as it is two notes.
				state.shapeParameter = (tone << 4) ^ (rate * 7919);

				// The bytes the sample actually holds, so the row shows the drum rather than a
				// stand-in for it. `sampleRate` carries the rate they are read at - a file of them
				// has to declare it or it plays at the wrong pitch - and it is taken here because
				// `frequency` is cleared for this channel a few lines below.
				//
				// Kept when the sample ends: the row is a record of the last hit, and blanking it
				// would leave an empty column for most of a drum line, where every hit lasts a few
				// milliseconds.
				if (bus != nullptr && bus->dpcm.valid)
				{
					state.sampleRate = dpcmRate (bus->dpcm.rateIndex, file.isPal ());

					const auto length = dpcmSampleLength ();

					// Keyed on the start count as well as the selection, because the same address
					// played twice reads different bytes if the driver moved the bank in between -
					// and keyed on the page count, because the length is "to the next sample" and
					// that boundary is unknown until the driver has pointed at the next one.
					if (dpcmWaveStart != bus->dpcm.start || dpcmWaveLength != length
						|| dpcmWaveStarts != bus->dpcmStarts || dpcmWavePages != bus->pageCount)
					{
						dpcmWave = dpcm::decode (bus->dpcm.bytes.data (), length);
						dpcmWaveStart = bus->dpcm.start;
						dpcmWaveLength = length;
						dpcmWaveStarts = bus->dpcmStarts;
						dpcmWavePages = bus->pageCount;
					}

					state.wave = dpcmWave;
				}
			}
			else
			{
				state.note = playback::noteForFrequency (state.frequency);
			}

			const int maxVolume = (std::max) (kLowestMaximumVolume, info->GetMaxVolume ());
			const float fromVolume = (float) (std::clamp) (info->GetVolume (), 0, maxVolume) / (float) maxVolume;

			// The triangle has no volume register - it is either sounding or not - and reports zero
			// there while still playing. Its output level is the honest number, and the same is
			// true of the noise and delta channels, so both are taken into account.
			const float fromOutput = (float) (std::clamp) ((int) info->GetOutput (), 0, kMixerMaximum)
										  / kMixerMaximumFloat;

			state.volume = (std::max) (fromVolume, fromOutput);

			// The triangle and the delta channel have no volume register: they are either sounding
			// at their fixed level or silent. Their instantaneous output can be zero at the moment
			// a block ends, which is why those rows showed an empty meter while they were playing.
			if (waveKind == kShapeTriangle || waveKind == kShapeDelta)
			{
				state.volume = state.active ? 1.0f : 0.0f;
			}

			// Neither of the two has a frequency worth reporting - a period setting and a sample
			// rate are not pitches - but both carry a note now, so only the frequency goes.
			if (waveKind == kShapeNoise || waveKind == kShapeDelta)
			{
				state.frequency = 0.0f;
			}

			// A channel that is not sounding has no note. The period register keeps its last value
			// after key-off, so a channel that had gone quiet still reported the note it stopped
			// on - and the keyboard kept that key lit, showing a pitch nobody could hear.
			if (! state.active)
			{
				state.note = -1;
				state.frequency = 0.0f;
				state.volume = 0.0f;		// the volume register also keeps its last value
			}
		}

		snapshot.channels.push_back (state);
		return snapshot.channels.size () - 1;
	};

	// The 2A03 is always there: two pulses on the APU, and triangle, noise and the delta channel
	// on the DMC. Both chips already report pitch, level and key state, so this is wiring rather
	// than new emulation - which is why the keyboard can show a NES song without a coprocessor.
	static const char* const pulseNames[] = { "Pulse 1", "Pulse 2" };
	static const char* const dmcNames[] = { "Triangle", "Noise", "DPCM" };

	// Duty comes from the pulse register's top two bits; the four settings are fixed by hardware.
	

	for (int i = 0; i < kPulseChannels; ++i)
	{
		const int setting = (apu->GetReg (i * kPulseRegisterStride) >> kDutyShift) & kDutyMask;
		addChannel (pulseNames[i], kChipName2A03, i, kShapePulse, kDutySettings[setting],
					apu->GetTrackInfo (i));
	}

	for (int i = 0; i < kDmcChannels; ++i)
	{
		addChannel (dmcNames[i], kChipName2A03, kFirstDmcTrack + i, kShapeTriangle + i, kNoDuty,
								dmc->GetTrackInfo (i));
	}

	// Four of the five expansion chips describe themselves through the same TrackInfo the 2A03
	// uses, so one table covers them: a name, the track it drives, which of the chip's own channels
	// to read, the shape to draw, and how many duty settings it has. Rows are in track order,
	// because a row and a track are the same thing to the mixer.
	struct ExpansionRow
	{
		const char* name;
		const char* chipName;
		int trackIndex;
		int chipChannel;
		int waveKind;

		/** The widths this channel's register selects from, or null for a fixed half.

			A table rather than a count: the VRC6's eight settings are sixteenths of a cycle and
			the MMC5's four are the 2A03's set, so "n of m" was two different fractions wearing one
			field. Dividing by the count drew the VRC6's 50 % pulse as a solid bar.
		*/
		const float* dutyTable;
		int dutyCount;

		bool levelImpliesKey;	// true when the chip says key-on for a channel it is not sounding
		float levelScale;		// the chip reports its volume against a wider register than it has
	};

	const auto addExpansionRows = [&] (const ExpansionRow* rows, int count, xgm::ISoundChip* chip)
	{
		for (int i = 0; i < count; ++i)
		{
			const auto& row = rows[i];

			if (chip == nullptr)
			{
				continue;
			}

			auto* info = chip->GetTrackInfo (row.chipChannel);

			float duty = kNoDuty;

			if (info != nullptr && row.dutyTable != nullptr)
			{
				const int setting = (std::clamp) (info->GetTone (), 0, row.dutyCount - 1);
				duty = row.dutyTable[setting];
			}

			const std::size_t index = addChannel (row.name, row.chipName, row.trackIndex,
												  row.waveKind, duty, info);

			// The S5B reports key-on for any channel its tone mask lets through, whatever the
			// volume register says - so a channel at zero volume lit a key nobody could hear.
			auto& state = snapshot.channels[index];

			state.volume = (std::min) (1.0f, state.volume * row.levelScale);

			if (row.levelImpliesKey && state.active && state.volume <= 0.0f)
			{
				state.active = false;
				state.note = -1;
				state.frequency = 0.0f;
			}
		}
	};

	static const ExpansionRow vrc6Rows[] =
	{
		// In the chip's own order, which is also the track order. The ramp's volume register is six
		// bits wide while the chip reports it against 255, so its meter read a quarter of what it
		// played; scaling by four puts the full 63 back at full scale.
		{ "Square 3", "VRC6", kFirstExpansionTrack + 0, 0, kShapePulse, kVrc6DutySettings, 8, false, 1.0f },
		{ "Square 4", "VRC6", kFirstExpansionTrack + 1, 1, kShapePulse, kVrc6DutySettings, 8, false, 1.0f },
		{ "Saw Wave", "VRC6", kFirstExpansionTrack + 2, 2, kShapeSaw, nullptr, 0, false, 4.0f }
	};

	static const ExpansionRow mmc5Rows[] =
	{
		{ "Square 1", "MMC5", kFirstExpansionTrack + 0, 0, kShapePulse, kDutySettings, 4, false, 1.0f },
		{ "Square 2", "MMC5", kFirstExpansionTrack + 1, 1, kShapePulse, kDutySettings, 4, false, 1.0f },
		// The PCM voice has neither pitch nor key: it plays whatever byte the CPU feeds it, and
		// its output is the sample's own level rather than a tone.
		{ "PCM", "MMC5", kFirstExpansionTrack + 2, 2, kShapeDelta, nullptr, 0, false, 1.0f }
	};

	static const ExpansionRow s5bRows[] =
	{
		// Three tone channels. The chip's noise and envelope are modes of these three rather than
		// voices of their own, which is why the track list has three rows and not five.
		{ "Square 1", "S5B", kFirstExpansionTrack + 0, 0, kShapePulse, nullptr, 0, true, 1.0f },
		{ "Square 2", "S5B", kFirstExpansionTrack + 1, 1, kShapePulse, nullptr, 0, true, 1.0f },
		{ "Square 3", "S5B", kFirstExpansionTrack + 2, 2, kShapePulse, nullptr, 0, true, 1.0f }
	};

	if (vrc6 != nullptr) { addExpansionRows (vrc6Rows, 3, vrc6.get ()); }
	if (mmc5 != nullptr) { addExpansionRows (mmc5Rows, 3, mmc5.get ()); }
	if (fme7 != nullptr) { addExpansionRows (s5bRows, 3, fme7.get ()); }

	if (fds != nullptr)
	{
		// The FDS reads its shape from a table the track info hands out a copy of: 64 six-bit
		// samples. Unipolar, so the DAC's midpoint - 31.5 - is the centre line to draw around.
		auto* info = dynamic_cast<xgm::TrackInfoFDS*> (fds->GetTrackInfo (0));

		const std::size_t index = snapshot.channels.size ();
		addChannel ("FDS", "FDS", kFirstExpansionTrack + 0, kShapeStored, kNoDuty, info);

		if (info != nullptr && includeWaves && snapshot.channels.size () > index)
		{
			auto& state = snapshot.channels[index];
			state.wave.reserve (64);

			for (int i = 0; i < 64; ++i)
			{
				state.wave.push_back ((float) (std::clamp) ((int) info->wave[i], 0, 63) / 31.5f - 1.0f);
			}
		}
	}

	if (vrc7 != nullptr)
	{
		// Six FM voices. There is no waveform to read - the chip computes its own - so the panel
		// synthesises one from the shape kind. The four numbers that make one patch look unlike
		// the next live inside the emulator's slot array, which is why they are asked for here
		// rather than derived from the registers ChannelState already carries.
		for (int i = 0; i < kVrc7Channels; ++i)
		{
			const std::string name = "FM " + std::to_string (i + 1);
			const auto index = addChannel (name.c_str (), "VRC7", kFirstExpansionTrack + i, kShapeFm,
										   kNoDuty, vrc7->GetTrackInfo (i));

			int multiple = 0;
			int level = 0;
			int waveSelect = 0;
			int feedback = 0;

			if (vrc7->GetFmParameters (i, multiple, level, waveSelect, feedback)
				&& index < snapshot.channels.size ())
			{
				snapshot.channels[index].shapeParameter
					= playback::waveform::packFm (multiple, level, waveSelect, feedback);
			}
		}
	}

	if (n106 == nullptr)
	{
		return true;
	}

	// Eight channels, in the order GetTrackInfo reports them - which is the reverse of the way the
	// hardware numbers them: the chip calls them 7 down to 0, so the row for channel 0 is the one a
	// NSF composer would call channel 8. The name follows the hardware, because the name is what a
	// musician reads; trackIndex keeps the API's order, because that is what the engine mutes.
	for (int channel = 0; channel < kN163Channels; ++channel)
	{
		auto* info = dynamic_cast<xgm::TrackInfoN106*> (n106->GetTrackInfo (channel));

		if (info == nullptr)
		{
			continue;
		}

		playback::ChannelState state;
		state.chipName = kChipNameN163;
		state.trackIndex = kFirstN163Track + channel;
		state.waveKind = kShapeStored;

		state.name = n163ChannelName (channel);
		state.active = info->GetKeyStatus ();
		state.frequency = (float) info->GetFreqHz ();
		state.note = state.active ? playback::noteForFrequency (state.frequency) : -1;

		const int maxVolume = (std::max) (kLowestMaximumVolume, info->GetMaxVolume ());
		state.volume = (float) (std::clamp) (info->GetVolume (), 0, maxVolume) / (float) maxVolume;

		const int length = (std::clamp) (info->wavelen, 0, kN163MaximumWaveLength);

		if (length > 0 && includeWaves)
		{
			state.wave.reserve ((std::size_t) length);

			// The chip stores four-bit samples; -1..1 is what every panel wants to draw.
			for (int i = 0; i < length; ++i)
			{
				state.wave.push_back ((float) info->wave[i] / kN163SampleMaximum * 2.0f - 1.0f);
			}
		}

		snapshot.channels.push_back (std::move (state));
	}

	return true;
}

void NsfPlayer::setOutputGain (float linear)
{
	outputGain = linear > 0.0f ? linear : 0.0f;
}

void NsfPlayer::setSilent (bool value)
{
	silent = value;
	applyMasks ();
}

uint64_t NsfPlayer::gateWrites (void) const
{
	return bus->gateWrites;
}

int NsfPlayer::gateValue (void) const
{
	return (int) bus->gateValue;
}

uint64_t NsfPlayer::executedClocks (void) const
{
	return clocksExecuted;
}

uint64_t NsfPlayer::ramWrites (void) const
{
	return bus->ramWriteCount;
}

int NsfPlayer::currentPC (void) const
{
	return (int) cpu->GetPC ();
}

int NsfPlayer::channelCount (void) const
{
	// Five on the 2A03 - the two the APU renders and the three the DMC object renders - plus the
	// expansion chip's own, of which a file can declare only one. The S5B answers with three and
	// not the five its GetTrackInfo can describe: the noise and the envelope are modes of those
	// three channels, and the mixer has no separate output to mute.
	if (vrc6) { return kFirstExpansionTrack + 3; }
	if (vrc7) { return kFirstExpansionTrack + kVrc7Channels; }
	if (fds) { return kFirstExpansionTrack + 1; }
	if (mmc5) { return kFirstExpansionTrack + 3; }
	if (fme7) { return kFirstExpansionTrack + 3; }
	if (n106) { return kFirstExpansionTrack + kN163Channels; }

	return kFirstExpansionTrack;
}

void NsfPlayer::setChipAudible (int device, bool audible)
{
	const int index = device <= 0 ? 0 : 1;

	if (chipAudible[index] == audible)
	{
		return;
	}

	chipAudible[index] = audible;
	applyMasks ();
}

void NsfPlayer::applyMasks (void)
{
	// Each chip mutes with a set bit over its own outputs, and the track list is the concatenation
	// of those outputs in this order: two from the APU, three from the DMC object, then the
	// expansion's. A solo is therefore one bit cleared in exactly one of these masks.
	const bool all = soloTrack < 0;

	const auto maskFor = [all, this] (int base, int count, bool chipMuted)
	{
		int mask = 0;

		for (int i = 0; i < count; ++i)
		{
			if (silent || chipMuted || (! all && soloTrack != base + i))
			{
				mask |= (1 << i);
			}
		}

		return mask;
	};

	apu->SetMask (maskFor (0, 2, ! chipAudible[0]));
	dmc->SetMask (maskFor (2, 3, ! chipAudible[0]));

	if (vrc6) { vrc6->SetMask (maskFor (5, 3, ! chipAudible[1])); }
	if (n106) { n106->SetMask (maskFor (5, 8, ! chipAudible[1])); }
	if (vrc7) { vrc7->SetMask (maskFor (5, 6, ! chipAudible[1])); }
	if (fme7) { fme7->SetMask (maskFor (5, 3, ! chipAudible[1])); }	// three tones; noise is a mode
	if (mmc5) { mmc5->SetMask (maskFor (5, 3, ! chipAudible[1])); }
	if (fds) { fds->SetMask (maskFor (5, 1, ! chipAudible[1])); }
}

int NsfPlayer::renderChannels (float* const* dest, int channelCount, int frames)
{
	if (frames <= 0 || dest == nullptr)
	{
		return 0;
	}

	const double clocksPerSample = (file.isPal () ? kPalClock : kNtscClock) / rate;
	const int usable = (std::min) (channelCount, 5);	// the two 2A03 objects, for now

	for (int i = 0; i < frames; ++i)
	{
		cpuClockRest += clocksPerSample;

		const auto clocks = (int) cpuClockRest;

		if (clocks > 0)
		{
			const int executed = cpu->Exec (clocks);
			cpuClockRest -= (double) executed;

			const auto tick = (uint32_t) (std::max) (0, executed);
			dmc->TickFrameSequence (tick);
			apu->Tick (tick);
			dmc->Tick (tick);
			if (vrc6) { vrc6->Tick (tick); }
			if (vrc7) { vrc7->Tick (tick); }
			if (n106) { n106->Tick (tick); }
			if (fme7) { fme7->Tick (tick); }
			if (fds) { fds->Tick (tick); }
			if (mmc5) { mmc5->Tick (tick); }

		}

		int32_t squares[4] = { 0, 0, 0, 0 };
		int32_t tnd[6] = { 0, 0, 0, 0, 0, 0 };

		apu->RenderChannels (squares, 2, 2);
		dmc->RenderChannels (tnd, 2, 3);

		for (int c = 0; c < 2 && c < usable; ++c)
		{
			dest[c][2 * i + 0] = (float) squares[c * 2 + 0] / 32768.0f * outputGain;
			dest[c][2 * i + 1] = (float) squares[c * 2 + 1] / 32768.0f * outputGain;
		}

		for (int c = 0; c < 3 && 2 + c < usable; ++c)
		{
			dest[2 + c][2 * i + 0] = (float) tnd[c * 2 + 0] / 32768.0f * outputGain;
			dest[2 + c][2 * i + 1] = (float) tnd[c * 2 + 1] / 32768.0f * outputGain;
		}

		// The expansion's channels, in the order the track list uses. FME7 has no RenderChannels
		// yet, so a song that declares one keeps its 2A03 tracks and leaves the rest silent here -
		// which is why those songs still take the one-player-per-track path in the backend.
		int32_t expansion[16] = {};

		if (vrc6) { vrc6->RenderChannels (expansion, 2, 3); }
		if (n106) { n106->RenderChannels (expansion, 2, 8); }
		if (vrc7) { vrc7->RenderChannels (expansion, 2, 6); }
		if (mmc5) { mmc5->RenderChannels (expansion, 2, 3); }
		if (fme7) { fme7->RenderChannels (expansion, 2, 3); }

		// The FDS is one voice: its whole output is its single channel.
		if (fds)
		{
			int32_t one[2] = { 0, 0 };
			fds->Render (one);
			expansion[0] = one[0];
			expansion[1] = one[1];
		}

		for (int c = 5; c < channelCount; ++c)
		{
			const int index = c - 5;

			dest[c][2 * i + 0] = (float) expansion[index * 2 + 0] / 32768.0f * outputGain;
			dest[c][2 * i + 1] = (float) expansion[index * 2 + 1] / 32768.0f * outputGain;
		}
	}

	rendered += (uint64_t) frames;
	return frames;
}

} // namespace toybox::backends::nsf
