#include "backends/nsf/NsfBackend.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "backends/nsf/NsfChannelNaming.h"
#include "backends/nsf/NsfPlayer.h"
#include "formats/nsf/NsfFile.h"
#include "settings/SettingsStore.h"

namespace toybox::backends::nsf
{

namespace
{

/** Three minutes, until the settings page has an answer. The header carries no length: an NSF's
	driver loops until it is told to stop, so the user is the only source of a duration. */
constexpr double kDefaultLengthSeconds = 180.0;

/** One rendered instance of the song. The emulation is the player's; this only adapts it. */
class NsfTrackPlayer final : public playback::ITrackPlayer
{
public:
	NsfTrackPlayer (const formats::nsf::NsfFile& file, double sampleRate, double seconds,
					std::shared_ptr<ScanHandle> scan)
		: player (file, sampleRate), length (seconds)
	{
		// Handed down from the song instance rather than looked up in a file-level table: this
		// player's scan is this song's scan, and the backend that owns both is the one that knows.
		player.setScanHandle (std::move (scan));
		player.start ();
	}

	void start (void) override {}
	void pause (void) override {}

	bool seekSeconds (double seconds) override
	{
		return player.seekTo (seconds);
	}
	void setTimeline (int, double) override {}
	bool captureState (playback::ChipStateSnapshot& snapshot) const override
	{
		return player.captureState (snapshot);
	}

	void setOutputGain (float linear) override
	{
		// The player already defaults to +8 dB; this is how the settings page overrides it.
		player.setOutputGain (linear);
	}

	void setTrackAudible (playback::TrackId track, bool audible) override
	{
		// One player per track: audible means this instance solos its own channel, and the
		// alternative is silence rather than the whole mix. The core mutes per channel bit, so
		// the other voices really are absent from this instance's output.
		if (audible && track >= 1)
		{
			player.setSilent (false);
			player.setTrackAudible ((int) track - 1);
		}
		else
		{
			player.setSilent (true);
		}
	}
	void setChipAudible (playback::DeviceId device, bool audible) override
	{
		player.setChipAudible ((int) device, audible);
	}

	int render (float* left, float* right, int numSamples) override
	{
		return player.render (left, right, numSamples);
	}

	/** One pass for every track: the chips render their channels separately, so the whole mixer is
		fed from a single emulation instead of one per track. */
	int renderTracks (float* const* buffers, int trackCount, int numSamples) override
	{
		return player.renderChannels (buffers, trackCount, numSamples);
	}

	double positionSeconds (void) override { return player.positionSeconds (); }

	bool isFinished (void) const override
	{
		// The driver loops forever; the length the user configured is what ends a song here.
		return player.positionSeconds () >= length;
	}

private:
	NsfPlayer player;
	double length = kDefaultLengthSeconds;
};

class NsfSongInstance final : public playback::ISongInstance
{
public:
	NsfSongInstance (formats::nsf::NsfFile image, std::vector<char> bytes)
		: file (std::move (image)), image (std::move (bytes))
	{
		detectUndeclaredChip ();

		info_.formatName = "NSF (NSFPlay core)";

		// The header has no length and the driver never ends, so the timeline comes from the
		// setting: three minutes unless the user says otherwise.
		length = (double) settings::SettingsStore::getInstance ().getNsfDurationSeconds ();
		info_.durationSeconds = length;
		info_.singlePassSeconds = length;

		// The song card reads upper-case keys, the way the libvgm backend publishes them; the
		// strings are already UTF-8 by the time the file is parsed.
		if (! file.name.empty ()) { info_.tags["TITLE"] = juce::String::fromUTF8 (file.name.c_str ()); }
		if (! file.artist.empty ()) { info_.tags["ARTIST"] = juce::String::fromUTF8 (file.artist.c_str ()); }
		if (! file.copyright.empty ())
		{
			info_.tags["COPYRIGHT"] = juce::String::fromUTF8 (file.copyright.c_str ());
		}
		info_.tags["SYSTEM"] = "Nintendo NES";

		info_.chips.push_back (playback::ChipInfo { "2A03", "NSFPlay", 0 });

		switch (file.chip ())
		{
			case formats::nsf::NsfChip::vrc6: info_.chips.push_back ({ "VRC6", "NSFPlay", 1 }); break;
			case formats::nsf::NsfChip::vrc7: info_.chips.push_back ({ "VRC7", "NSFPlay", 1 }); break;
			case formats::nsf::NsfChip::fds:  info_.chips.push_back ({ "FDS",  "NSFPlay", 1 }); break;
			case formats::nsf::NsfChip::mmc5: info_.chips.push_back ({ "MMC5", "NSFPlay", 1 }); break;
			case formats::nsf::NsfChip::fme7: info_.chips.push_back ({ "S5B",  "NSFPlay", 1 }); break;
			case formats::nsf::NsfChip::n163: info_.chips.push_back ({ "N163", "NSFPlay", 1 }); break;

			// Every chip above has a row here or it does not exist as far as the chip card is
			// concerned, however many tracks it contributes. The N163 was the one missing, and a
			// file that declares it showed a card with the 2A03 alone while the mixer drew all
			// eight of its channels underneath.
			default: break;
		}

		// Five channels, because the two chip objects expose exactly that many outputs between them:
		// the APU renders the two pulses, and the DMC object renders triangle, noise and DPCM.

		// Every channel the emulation renders separately becomes a track, including the expansion
		// chip's: each of them has its own mask bit, so isolating one is a matter of the player
		// clearing a single bit rather than of the backend guessing.
		static const char* const apuNames[] = { "Pulse 1", "Pulse 2" };
		static const char* const dmcNames[] = { "Triangle", "Noise", "DPCM" };

		playback::TrackId nextId = 1;
		int channel = 0;

		const auto addTrack = [&] (const char* name, const char* group, bool melodic, playback::DeviceId device)
		{
			tracks_.push_back (playback::TrackInfo { nextId++, device, (int) device, channel++, true, name, group,
												 melodic ? playback::MidiCapability::melodic
														 : playback::MidiCapability::unavailable });
		};

		// The two sample voices answer with a note now: the noise channel's sixteen periods are
		// laid out as sixteen semitones, and a DPCM sample keeps the slot the scan gave it. Neither
		// carries a pitch to slide, so neither is melodic - what they are is a fixed set of notes a
		// drum kit can be built on.
		const auto addSampleTrack = [&] (const char* name, const char* group)
		{
			tracks_.push_back (playback::TrackInfo { nextId++, (playback::DeviceId) 0, 0, channel++, true, name,
													  group, playback::MidiCapability::discrete });
		};

		for (const char* name : apuNames) { addTrack (name, "2A03", true, 0); }

		// The triangle has a period register and a wavetable: it has a pitch, unlike the noise and
		// the delta channel that share its object. Calling it unavailable kept the one 2A03 voice a
		// bass line is written for out of every MIDI menu.
		// "DPCM", not "DMC": the community names the voice after what it plays, and the DMC is
		// the object that plays it. A strip labelled with the chip reads like a second device.
		addTrack (dmcNames[0], "2A03", true, 0);
		addSampleTrack (dmcNames[1], "2A03");
		addSampleTrack (dmcNames[2], "2A03");

		switch (file.chip ())
		{
			case formats::nsf::NsfChip::vrc6:
				// Track order is the chip's channel order: the per-channel render writes channel 0
				// into the first expansion track, and channel 0 is the first pulse. Listing the ramp
				// first - as this did - put each track's name on a different voice than the one it
				// carried, which the mute buttons and the keyboard both read.
				addTrack ("Square 3", "VRC6", true, 1);
				addTrack ("Square 4", "VRC6", true, 1);
				addTrack ("Saw Wave", "VRC6", true, 1);
				break;

			case formats::nsf::NsfChip::vrc7:
				for (int i = 0; i < 6; ++i)
				{
					addTrack (juce::String ("FM " + juce::String (i + 1)).toRawUTF8 (), "VRC7", true, 1);
				}
				break;

			case formats::nsf::NsfChip::n163:
				for (int i = 0; i < 8; ++i)
				{
					addTrack (n163ChannelName (i).c_str (), "N163", true, 1);
				}
				break;

			case formats::nsf::NsfChip::fds:
				addTrack ("FDS", "FDS", true, 1);
				break;

			case formats::nsf::NsfChip::mmc5:
				// two pulse channels and the PCM voice; the nonlinear mixer in the chip is where
				// the three outputs come from
				addTrack ("Square 1", "MMC5", true, 1);
				addTrack ("Square 2", "MMC5", true, 1);
				addTrack ("PCM", "MMC5", false, 1);
				break;

			case formats::nsf::NsfChip::fme7:
				// Three, not five: the S5B has three tone channels, and the noise and the envelope
				// are modes of those channels rather than voices with an output of their own.
				for (int i = 0; i < 3; ++i)
				{
					addTrack (juce::String ("Square " + juce::String (i + 1)).toRawUTF8 (), "S5B", true, 1);
				}
				break;

			default:
				break;
		}
		// A file walked in an earlier session needs no thread at all: the cache goes straight into
		// this song's handle, and the player created below finds it already there. scanFinished
		// has to be set here as well - nothing else will, and "no thread" would otherwise read as
		// "still scanning" for everyone who asks.
		if (NsfPlayer::adoptCachedScan (file, *scanHandle))
		{
			adoptFromHandle ();
			scanFinished.store (true, std::memory_order_release);
		}
		else
		{
			// Started last, not first. A thread launched before the rest of this constructor ran
			// would outlive a constructor that threw: the destructor does not run for an object
			// that was never constructed, so nothing would ever set the cancel flag, and the scan
			// would go on reading members of an object that had already been freed.
			scanner = std::thread ([this] { runScan (); });
		}
	}

	~NsfSongInstance () override
	{
		// A scan in flight keeps its own emulation and would outlive this object. Asking it to stop
		// and waiting is the only way to know the thread has finished touching anything - and the
		// scan checks the flag once per block, so the wait is bounded by one block, not by the song.
		cancelScan.store (true);

		if (scanner.joinable ())
		{
			scanner.join ();
		}
	}

	const playback::SongInfo& getInfo (void) const override { return info_; }
	const std::vector<playback::TrackInfo>& getTracks (void) const override { return tracks_; }

	const playback::MidiSequence* getMidiSequence (void) const override
	{
		// Whatever the scan has produced so far: null until it lands, the real stream afterwards.
		// Published once and never replaced, so the pointer the engine caches stays valid for as
		// long as this instance does - see runScan().
		std::lock_guard<std::mutex> guard (streamMutex);
		return midiSequence.get ();
	}

	bool isAnalysing (void) const override
	{
		// Not "no stream yet": a scan can end without one - cancelled, or the song plays nothing
		// worth numbering - and asking the stream would then say "analysing" for as long as the
		// file was open. This is the flag the scan sets when it stops, however it stops.
		return ! scanFinished.load (std::memory_order_acquire);
	}

	playback::BackendCapabilities getCapabilities (void) const override
	{
		playback::BackendCapabilities caps;
		// A song without an expansion has every channel split by the core, so one emulation can feed
		// the whole mixer. With an expansion only the 2A03's five are split - the other chips have
		// no per-channel render yet - so those songs stay on one player per track rather than
		// offering tracks that would be silent.
		// One player per track, for every file. Muting a channel is what the core supports; making
		// one emulation render every channel separately is what it does not, and the attempt to
		// fake it cost more than the extra instances do.
		caps.isolation = playback::IsolationMode::multiInstance;
		caps.canSeek = true;
		caps.rendersAtHostRate = true;
		// Every chip the emulation renders can be silenced on its own: the core mutes per output
		// bit, and setChipAudible() turns a whole group of those bits off at once.
		caps.canMuteChips = true;

		// An NSF has no register stream to read - it is a program - so the sequence comes from
		// running that program once with the output thrown away. That pass is the same one that
		// numbers the DPCM samples, and it is cached on the file's bytes.
		caps.canEmitMidi = true;
		return caps;
	}

	bool prepare (double hostSampleRate, int maxBlockSize, juce::String& errorMessage) override
	{
		rate = hostSampleRate > 0.0 ? hostSampleRate : 44100.0;
		length = (double) settings::SettingsStore::getInstance ().getNsfDurationSeconds ();
		blockSize = maxBlockSize > 0 ? maxBlockSize : 512;
		errorMessage.clear ();
		return true;
	}

	void setTimeline (int, double) override {}

	std::unique_ptr<playback::ITrackPlayer> createPlayer (juce::String& errorMessage) override
	{
		errorMessage.clear ();
		return std::make_unique<NsfTrackPlayer> (file, rate, length, scanHandle);
	}

	/** Runs the driver for a moment and, if it writes to a chip its header never mentioned, turns
		that chip on. Two of the files this was written for do exactly that: they declare chip_flags
		0x00 and drive a VRC6 regardless. */
	void detectUndeclaredChip (void)
	{
		if (file.chip () != formats::nsf::NsfChip::none)
		{
			return;
		}

		NsfPlayer probe (file, rate);
		probe.skipPrescan ();		// only the write counters are read below
		probe.start ();

		float left [512] = {};
		float right [512] = {};

		for (int block = 0; block < 44; ++block)   // half a second at 44.1 kHz
		{
			probe.render (left, right, 512);
		}

		struct { int index; std::uint8_t flag; } const candidates[] =
		{
			{ 0, 0x01 },	// VRC6
			{ 2, 0x10 },	// N163
			{ 3, 0x20 },	// FME7
			{ 1, 0x02 }		// VRC7
		};

		for (const auto& candidate : candidates)
		{
			if (probe.expansionWrites (candidate.index) > 0)
			{
				file.chipFlags |= candidate.flag;
				return;
			}
		}
	}

private:
	formats::nsf::NsfFile file;
	std::vector<char> image;
	playback::SongInfo info_;
	std::vector<playback::TrackInfo> tracks_;
	/** What the host asked for, until it has asked. Every probe in this file uses this rather
		than a literal, so a scan and the playback it feeds describe time the same way. */
	double rate = 44100.0;
	double length = kDefaultLengthSeconds;
	bool expansionPresent = false;
	int blockSize = 512;

	/** Walks the song on its own thread: numbering the DPCM samples and recording every note. The
		file is playable the whole time, and the DPCM row shows "analysing" until it lands. */
	std::thread scanner;
	std::atomic<bool> cancelScan { false };

	/** This song's scan: written by the scanner thread, read by the player that renders audio and
		by getMidiSequence(). Both players in a song hold the same handle, which is what replaced
		the file-level table they used to meet in. */
	std::shared_ptr<ScanHandle> scanHandle = std::make_shared<ScanHandle> ();

	/** Guards midiSequence, which the scanner writes and getMidiSequence() reads. */
	mutable std::mutex streamMutex;
	std::shared_ptr<const playback::MidiSequence> midiSequence;

	/** True once the scan has stopped, however it stopped.

		Kept apart from `midiSequence` because the two answer different questions: a scan that was
		cancelled, or that found nothing to publish, ends with no stream - and reading "still
		working" out of "no stream" left the song saying "analysing" forever.
	*/
	std::atomic<bool> scanFinished { false };

	/** Takes the stream out of the handle. Both paths end here: the scan publishes into it, and a
		cache hit fills it before the constructor is done. */
	void adoptFromHandle (void)
	{
		if (auto scan = scanHandle->snapshot ())
		{
			std::lock_guard<std::mutex> guard (streamMutex);
			midiSequence = scan->midiSequence;
		}
	}

	void runScan (void)
	{
		// One publication, from null to non-null: the engine holds this raw pointer across blocks
		// and it is never replaced, so there is no retired list to keep.
		try
		{
			// A player of its own: scanning resets the CPU and runs it to the end of the song,
			// which would wreck playback if it shared the emulation the mixer renders from. It
			// publishes into the song's handle, which is how the player rendering audio hears
			// about it.
			NsfPlayer probe (file, rate);
			probe.setScanHandle (scanHandle);
			probe.setScanCancelFlag (&cancelScan);
			probe.scanSong ();

			adoptFromHandle ();
		}
		catch (...)
		{
			// An exception escaping a thread takes the whole process with it. Nothing here is
			// expected to throw, but leaving the stream empty and reporting the scan finished is
			// the one failure this file can survive.
		}

		scanFinished.store (true, std::memory_order_release);
	}
};

} // namespace

bool NsfBackend::canOpen (const playback::SongMemory& memory) const
{
	return memory.isValid () && formats::nsf::NsfFile::looksLikeNsf (memory.data, memory.size);
}

std::unique_ptr<playback::ISongInstance> NsfBackend::open (const playback::SongMemory& memory,
														   juce::String& errorMessage)
{
	if (! memory.isValid ())
	{
		errorMessage = "empty song image";
		return nullptr;
	}

	formats::nsf::NsfFile file;
	std::string error;

	if (! formats::nsf::NsfFile::parse (memory.data, memory.size, file, error))
	{
		errorMessage = juce::String (error);
		return nullptr;
	}

	// The image is copied because the core's memory block outlives the caller's buffer, and the
	// players of a song read from the same copy.
	std::vector<char> bytes (static_cast<const char*> (memory.data),
							 static_cast<const char*> (memory.data) + memory.size);

	return std::make_unique<NsfSongInstance> (std::move (file), std::move (bytes));
}

} // namespace toybox::backends::nsf
