#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include <juce_core/juce_core.h>

#include "playback/DawSync.h"
#include "playback/MidiSink.h"
#include "playback/OutputRouting.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "playback/SongBackend.h"

namespace toybox::playback
{
struct SessionState;	// playback/SessionState.h: the project state, as plain values
}

namespace toybox
{

/** Metadata of one sound chip used by the current song (the user's mute toggle lives here). */
struct ChipEntry
{
	juce::String name;
	juce::String core;
	playback::DeviceId deviceId {};		// the backend's opaque device handle
	bool muted = false;					// muted by the user
};

/**
	The playback engine: drives a format backend through the playback port and adds the mixer.

	No format engine is ever touched directly - `playback::BackendRegistry` picks a backend for
	the loaded image and everything below that goes through `ISongInstance` / `ITrackPlayer`
	(Source/playback/SongBackend.h). Swapping or adding a format engine therefore never touches
	this class, the UI or the host integration: see docs/desgin/playback-backend-design.md.

	Plain player mode renders through a single, un-isolated player - the classic behaviour.
	Mixer mode renders one isolated player per track of the song and sums them with per-track
	gain / pan / mute / solo applied in the mix, which turns every chip channel into a mixer
	track. Both modes share the file image and all song metadata; switching the mode rebuilds
	the players at the current position.

	Threading stays as before: all access to the players is serialised through one lock. The
	message thread takes it for commands and for (re)building the players, the audio thread
	try-locks at the top of render() and outputs silence when a command is in flight. Everything
	the UI polls at 15/10 Hz is served lock-free - position/peaks are atomics, duration and loop
	point are cached song facts, tags and the track list are message-thread data (the track
	parameters are plain atomics the mix reads without touching the lock). A failed try-lock
	costs a whole silent block, which is audible as a faint tick.
*/
class PlaybackEngine
{
public:
	PlaybackEngine (void);
	~PlaybackEngine (void);

	// --- message thread ---
	/** Loads a song file and starts playback. Returns false and fills errorMessage on failure. */
	bool loadFile (const juce::File& file, juce::String& errorMessage);
	void unloadFile (void);

	/** Called on the message thread when an asynchronous load finishes. */
	using LoadCallback = std::function<void (bool ok, const juce::String& errorMessage)>;

	/** Loads `file` without doing the work on the calling thread (review M-1).

		The read, the unpacking and the build run on a worker owned by the engine; the finished
		song is installed on the message thread, where every other mutator lives. The window keeps
		drawing, and the song that is still current keeps playing, until the swap.

		`onDone` is called on the message thread exactly once - or not at all when a newer load
		(or unloadFile()) supersedes this one, in which case that request is the one that reports.
		The callback must not be called from the worker: the UI it touches is message-thread data.

		`startPlaying` installs the transport with the swap (the default, and what opening a file
		means). A restore passes false: the song is there to carry on with, but the application
		did not ask for sound the moment its window appeared.
	*/
	void loadFileAsync (const juce::File& file, LoadCallback onDone, bool startPlaying = true);
	/** Sets the output sample rate / maximum block size (host sample rate). */
	void prepare (double newSampleRate, int maxBlockSize);

	void play (void);
	void pause (void);
	void stop (void);
	void setPositionSeconds (double seconds);

	/** Same request, but the replay happens on a worker and this returns at once. The transport uses
		it because a seek can take seconds on a backend that replays every track; anything that needs
		to read the players straight afterwards must use setPositionSeconds() instead. */
	void requestPositionSeconds (double seconds);
	void setLoopCount (int loops);			// 0 = loop forever
	void setFadeSeconds (double seconds);
	/** The output volume: the **last** gain stage of the engine, applied after every mixer
		track and after the mixer's own master gain. Lock-free (one atomic). */
	void setMasterVolume (float linear);	// 1.0 = 100 %
	/** The mixer's master gain - the master strip's fader. It scales the sum of the tracks
		and sits *before* the output volume, so both controls are independent stages. */
	void setMixerMasterGain (float linear);	// 1.0 = 100 %

	/** A gain in the format's own output, not in the mix. Formats whose synthesis has headroom -
		the NES is the example - pick this up; everything else ignores it. It scales the material
		before the per-track faders, so the meters show what is actually heard. */
	void setSourceGain (float linear);
	void setChipMuted (size_t chipIndex, bool shouldBeMuted);

	/** Removes the DC offset the chips' DACs leave in the signal.

		Several of the old sound chips have an output that sits above zero when they are silent -
		a VGM of the Sega PSG renders with a mean around 0.12 - which eats headroom and can click
		when a song starts. Off by default: it changes what the file sounds like, and the tests
		measure the signal as the chip produced it.
	*/
	void setDcBlockEnabled (bool shouldBeEnabled);
	bool isDcBlockEnabled (void) const { return dcBlockEnabled.load(); }

	/** Whether this backend can silence a chip on its own - the chips card is only shown when it
		can, because a checkbox that changes nothing is worse than no checkbox. */
	bool canMuteChips (void) const { return chipsMuteable.load(); }

	/** Whether the loaded song has a MIDI sequence at all. The mixer asks before offering a MIDI
		channel: a track's capability describes its voice, not whether anything can be sent. */
	bool canEmitMidi (void) const { return midiEmittable.load(); }

	/** True while the open song's MIDI stream is still being produced on a background thread.

		A format that has to be run to be understood - an NSF - cannot answer at load time, so it
		answers later. This is how a caller asks whether "no notes yet" means "still working" or
		"this song has none".
	*/
	bool isAnalysing (void) const;

	/** Waits up to `timeoutMs` for that work to finish. True when nothing is outstanding. */
	bool waitForAnalysis (int timeoutMs) const;

	bool isLoaded (void) const { return loaded; }
	bool isPlaying (void) const { return playing.load(); }
	/** Whether the last local transport action was pause rather than stop or load. The button
		lights read this instead of keeping a copy: a pause that came from the tray menu and one
		that came from the Pause button are the same state. Play, stop and a load clear it. */
	bool isPaused (void) const { return paused.load(); }
	bool isFinished (void) const { return finished.load(); }
	/** Lock-free snapshot of the playback position: render() publishes it once per block and
		every seek updates it, so the UI polling never contends for the render lock. */
	double getPositionSeconds (void) const { return positionSeconds.load(); }
	/** Song length, cached while the lock is held when the file, the loop count, the fade time
		or the sample rate changes - read without the lock. */
	double getDurationSeconds (void) const { return cachedTimeline.durationSeconds; }
	/** Position of the loop point inside a single playthrough, or a negative value if the song
		has no loop point (or nothing is loaded). */
	double getLoopPointSeconds (void) const
	{
		return cachedTimeline.hasLoop() ? cachedTimeline.loopStartSeconds() : -1.0;
	}
	int getLoopCount (void) const { return loopCount; }
	double getFadeSeconds (void) const { return fadeSeconds; }

	// --- DAW sync (one-way: the host's clock is the master - playback/DawSync.h) ---
	/** The sync state the processor publishes the host clock into and the editor edits; it lives
		here because render() is what follows it, and because it belongs in the project state.
		(The song facts the mapping needs stay private: they are read by followDawTransport() under
		the same lock that writes them, and a public copy would invite reading them without it.) */
	playback::DawSync& getDawSync (void) { return dawSync; }
	float getMasterVolume (void) const { return masterVolume.load(); }

	/** Copies the most recent output for display - the waveform of what is being heard - with the
		newest sample last. The audio thread writes it without a lock and this reads it the same way:
		a sample torn in half is one wrong pixel, never a wrong sound. Returns the count written. */
	int readScope (float* dest, int maxSamples) const;

	/** Copies the last state the audio thread captured. Never blocks the audio thread: it tries a
		lock, and answering "nothing right now" is better than making a block of audio wait. */
	bool captureChipState (playback::ChipStateSnapshot& snapshot) const;

	/** Tells the engine whether anyone is looking. While true, each rendered block ends with a
		capture; while false it costs one atomic read per block. A page that is not on screen must
		turn this off, or the capture runs for nobody. */
	void setStateCaptureWanted (bool wanted);
	float getMixerMasterGain (void) const { return mixerMasterGain.load(); }
	const juce::File& getFile (void) const { return currentFile; }
	juce::String getTag (const juce::String& tagName) const;
	const std::vector<ChipEntry>& getChips (void) const { return chips; }

	// --- project state (the host's session) ---
	/** Writes everything a project has to keep - the song bytes and every mixer value - into one
		gzip-compressed XML blob. The snapshot is taken under the lock (a 16 MB copy at worst); the
		encoding, which is the slow part, happens outside it. */
	void writeState (juce::MemoryBlock& destData) const;

	/** Restores a blob written by writeState(). An older format, a different version or a
		truncated file returns false with errorMessage filled in and leaves the engine untouched.
		A state that decodes but whose song cannot be started behaves like a failed load: playback
		stops, the settings that came with it stay applied. The song comes back **stopped, at the
		top** - a host opening a project must not make sound on its own. Message thread, like every
		other mutator here (a host calling this from its own thread is M-2 in the review).

		The order is not obvious: the mixer mode and the routing scheme have to be in place
		*before* the song is built (they decide how many players exist and where the tracks go),
		the per-track values only *after* it (the load replaces the track list). */
	bool readState (const void* data, int sizeInBytes, juce::String& errorMessage);

	/** The loaded image as it was read - a .vgz stays packed. Empty when nothing is loaded. */
	juce::MemoryBlock getSongImage (void) const;

	// --- mixer ---
	/** Mixer mode renders one isolated track per channel; off means the plain single player. */
	bool isMixerEnabled (void) const { return mixerEnabled; }
	/** Rebuilds the players for the other mode, keeping the position and the playing state. */
	void setMixerEnabled (bool shouldBeEnabled);

	/** One entry per track the backend reports - the list exists in both modes. */
	const std::vector<playback::TrackInfo>& getTracks (void) const { return trackLayout; }

	/** Where each track's notes got their pitch - the audit behind the "transpose by the sample's
		root" setting (design doc section 15).

		`rooted` counts the notes that carry a measured root; `distinctRoots` says how many different
		values they took. More than one means the same track measured different roots for different
		notes - the sample at two rates should measure the same root, so a track with several is the
		sign that a measurement picked a different window per key-on and cannot be trusted. */
	struct TrackPitchSummary
	{
		int notes = 0;
		int registerExact = 0;
		int sampleInferred = 0;
		int unpitched = 0;
		int distinctRoots = 0;
	};

	/** One entry per track, in `getTracks()` order. Empty until a song is loaded. */
	std::vector<TrackPitchSummary> getPitchSummary (void) const;

	/** Track parameters. They are stored in atomics, so the audio thread reads them without
		ever contending for the engine lock. Note that in plain player mode the mix is not
		applied (the single player renders the whole song) - the values simply persist. */
	void setTrackGain (size_t trackIndex, float linearGain);
	float getTrackGain (size_t trackIndex) const;
	void setTrackPan (size_t trackIndex, float pan);	// -1 = left, 0 = centre, +1 = right
	float getTrackPan (size_t trackIndex) const;
	void setTrackMute (size_t trackIndex, bool shouldBeMuted);
	bool isTrackMuted (size_t trackIndex) const;
	void setTrackSolo (size_t trackIndex, bool shouldBeSoloed);
	bool isTrackSoloed (size_t trackIndex) const;
	/** Peak level of the track's isolated output in the last rendered block (0 = L, 1 = R). */
	float getTrackPeak (size_t trackIndex, int channel) const;
	/** Peak level of the final mix in the last rendered block (0 = L, 1 = R). */
	float getOutputPeak (int channel) const;
	/** Position of one track's player - used to verify that the tracks stay in sync. */
	double getTrackPositionSeconds (size_t trackIndex) const;

	/** Blocks the audio thread had to skip because a command held the lock. Diagnostics: the core
		test uses it to tell "a load never blocked the audio thread" from "it did", which is otherwise
		invisible - render() just returns silence and nobody notices (review H-1). */
	int getStarvedBlocks (void) const { return starvedBlocks.load(); }

	// --- output routing ---
	/** The routing scheme **newly loaded songs** start with - the mixer card's two shortcut
		buttons set it. Applying it also re-routes the song that is loaded right now. */
	void setDefaultRouting (playback::DefaultRouting mode);
	playback::DefaultRouting getDefaultRouting (void) const { return defaultRouting; }

	/** Destination of one track: `kMasterOutput` (the default) or a real bus index. Values
		outside the declared range are clamped to the master bus. Lock-free (one atomic). */
	void setTrackOutput (size_t trackIndex, playback::OutputTarget target);
	playback::OutputTarget getTrackOutput (size_t trackIndex) const;

	/** Destination of the master bus itself - a real bus, never the master (clamped to bus 0). */
	void setMasterOutput (playback::OutputTarget busIndex);
	playback::OutputTarget getMasterOutput (void) const { return masterOutput.load(); }

	// --- MIDI routing (the event generation lives in the backend; the engine only carries the
	// routing state and the per-block sink that turns it into MIDI messages) -------------------
	/** MIDI channel of one track: 0 = Off, 1..16. Lock-free (one atomic). */
	void setTrackMidiChannel (size_t trackIndex, int channel);
	int getTrackMidiChannel (size_t trackIndex) const;
	/** Master MIDI channel: 0 = Off, 1..16. */
	void setMasterMidiChannel (int channel)
	{
		masterMidiChannel.store (juce::jlimit (0, 16, channel));
		midiResetGeneration.fetch_add (1);
	}
	int getMasterMidiChannel (void) const { return masterMidiChannel.load(); }

	/** How far the receiving instrument bends, in semitones each way (1..24, default 12).

		The range is the instrument's setting, not ours: nothing in MIDI reports it, so the engine
		scales every bend by this value and declares it as RPN 0 when playback (re)starts or a
		channel comes into use. Changing it while playing re-declares it on the next block; notes
		already sounding keep the bend value sent under the old range until the next bend on their
		channel, because cutting them on every step of a slider drag would be worse than the
		momentary mismatch. */
	void setMidiPitchBendRange (int semitones)
	{
		midiPitchBendRange.store (juce::jlimit (playback::minPitchBendRange, playback::maxPitchBendRange,
												semitones));
	}
	int getMidiPitchBendRange (void) const { return midiPitchBendRange.load(); }

	/** Whether the chips' fine pitch becomes pitch bend at all. Off: every note keeps its nearest
		semitone and no bend message is sent (the sequence's bends are dropped, not centred). */
	void setMidiPitchBendEnabled (bool shouldSendBends) { midiPitchBendEnabled.store (shouldSendBends); }
	bool isMidiPitchBendEnabled (void) const { return midiPitchBendEnabled.load(); }

	/** Mutes what the master bus contributes to its target bus (direct outputs keep playing). */
	void setMasterMuted (bool shouldBeMuted)
	{
		masterMuted.store (shouldBeMuted);
		midiResetGeneration.fetch_add (1);
	}
	bool isMasterMuted (void) const { return masterMuted.load(); }

	/** Which buses the host connected in the last block (published by render(), read lock-free). */
	bool isOutputBusConnected (int busIndex) const;

	// --- audio thread ---
	/** Renders into the host's bus buffers (`numBuses` must be `kOutputBusCount`). The optional
		sink receives this block's MIDI events; it is called on the audio thread. */
	void render (const playback::OutputBusBuffers* buses, int numBuses, int numSamples,
				 playback::MidiBlockSink* midiSink = nullptr);

private:
	struct TrackState
	{
		std::atomic<float> gain { 1.0f };
		std::atomic<float> pan { 0.0f };
		std::atomic<bool> mute { false };
		std::atomic<bool> solo { false };
		std::atomic<float> peakL { 0.0f };
		std::atomic<float> peakR { 0.0f };
		std::atomic<playback::OutputTarget> output { playback::kMasterOutput };
		std::atomic<int> midiChannel { 0 };		// 0 = Off, 1..16
	};

	/** Audio thread, lock-free: report silence on the meters (see render()). */
	void clearTrackPeaks (void);
	void clearOutputPeaks (void);

	double getPlayLengthSeconds (void) const;			// caller must hold the lock
	void refreshCachedTimes (void);						// caller must hold the lock
	/** The seek itself, without the lock: the public entry point takes it and calls this, and so
		does rebuildPlayers() - which runs with the lock already held. Named this way so the
		convention is visible instead of relying on the lock being recursive. */
	void setPositionSecondsLocked (double seconds);		// caller must hold the lock
	void seekAllPlayersTo (double target);				// caller must hold the lock
	void runSeekWorker (void);
	void updatePlayerTimeline (void);					// caller must hold the lock
	void emitMidiForBlock (double blockStartSeconds, int numSamples, double sampleRate,
						   playback::MidiBlockSink* sink);	// caller must hold the lock
	/** Ends every note this engine has started and not yet ended: one note-off each, then All
		Notes Off on the channels that had one. The set is cleared, so the messages go out once
		and an idle block that follows sends nothing. Audio thread only, no lock needed. */
	void flushActiveNotesOff (playback::MidiBlockSink* sink, int sampleOffset);
	void applyChipMutes (void);							// caller must hold the lock
	/** Sets every track (and a new song's tracks) to the current routing scheme. */
	void applyDefaultRouting (void);					// caller must hold the lock
	void destroyPlayers (void);
	void resetState (void);

	/** Everything a load produces, built outside the lock and moved into the engine under it.

		The members mirror the engine's own state one for one; buildSong() fills this instead of
		touching the engine, which is what lets the slow part of a load - unpacking a .vgz, starting
		the sound cores - run while the audio thread keeps rendering the song that is still current
		(review H-1). */
	struct LoadedSong
	{
		std::unique_ptr<playback::ISongInstance> song;
		std::vector<std::unique_ptr<playback::ITrackPlayer>> players;
		std::vector<playback::TrackInfo> layout;
		std::vector<std::unique_ptr<TrackState>> tracks;
		int soloCount = 0;
		std::vector<ChipEntry> chips;
		std::map<juce::String, juce::String> tags;
		playback::SongTimeline timeline;
		const playback::MidiSequence* midiSequence = nullptr;
	/** The backend renders every track in one pass, so one player serves all of them. */
	bool inPlace = false;
	};

	/** Everything buildSong() reads, copied from the engine on the message thread so that a build
		running on a worker never touches a member render() can see. */
	struct BuildConfig
	{
		double sampleRate = 44100.0;
		int maxBlockSize = 512;
		int loopCount = 1;
		double fadeSeconds = 3.0;
		bool mixerEnabled = true;
		playback::DefaultRouting defaultRouting = playback::DefaultRouting::master;
		std::vector<ChipEntry> chips;
	};

	/** Copies the live configuration into a BuildConfig. Message thread. */
	BuildConfig makeBuildConfig (void) const;

	/** Builds a song from an image **without touching engine state**, so it needs no lock and can
		run anywhere: it reads the passed configuration only and returns everything a load
		produces. `song` is null on failure. */
	static LoadedSong buildSong (const BuildConfig& config, const juce::MemoryBlock& image,
								 const juce::File& file, juce::String& errorMessage);

	/** Swaps a built song into the engine: the only part of a load that has to run under the
		lock. The image is moved in (the engine keeps it for the session), the position goes back
		to zero and `startPlaying` is installed with the swap, not set afterwards. */
	bool installLoaded (LoadedSong&& built, juce::MemoryBlock&& image, const juce::File& file,
						bool startPlaying);

	/** The message-thread half of loadFileAsync(): drops the result when a newer load has taken
		over in the meantime, otherwise installs it and reports. */
	void finishLoad (int generation, LoadedSong&& built, juce::MemoryBlock&& image,
					 const juce::File& file, bool ok, const juce::String& errorMessage,
					 bool startPlaying, const LoadCallback& onDone);

	/** Moves a built song into the engine and publishes its state - the only part of a load that has
		to happen under the lock. Leaves the position at zero; the callers decide what happens to the
		transport. */
	void installSong (LoadedSong&& built);				// caller must hold the lock

	/** Rebuilds the players from the file that is already loaded, keeping the position, the
		transport state and the mixer values the user set. Takes the lock only to install, so a
		mode switch or a sample-rate change does not stall the audio thread either. */
	void rebuildPlayers (juce::String& errorMessage);

	/** The body of both loadFile() and readState(): builds outside the lock, swaps under it. The
		image is taken by value because the engine keeps its own copy for the rest of the session
		(the backend gets a view of it only while building). `file` supplies the name - the
		extension is what picks the backend - and is never read from disk. `startPlaying` is
		installed with the swap rather than set afterwards, so a restored project cannot leak one
		render block of sound before the transport is stopped again. */
	bool loadImage (juce::MemoryBlock image, const juce::File& file, bool startPlaying,
					juce::String& errorMessage);

	/** The shared bodies: used both by buildSong() (on a song that is not installed yet) and by the
		member functions below (on the installed one), which is why they take their state as
		parameters instead of reading members. */
	static void applyTrackIsolationTo (bool mixerEnabled,
									   const std::vector<playback::TrackInfo>& layout,
									   std::vector<std::unique_ptr<playback::ITrackPlayer>>& players);
	static void applyChipMutesTo (const std::vector<ChipEntry>& chips,
								  const std::vector<std::unique_ptr<playback::ITrackPlayer>>& players);
	static void applyRoutingTo (playback::DefaultRouting routing,
								std::vector<std::unique_ptr<TrackState>>& tracks);
	/** Copies the song's length facts into a timeline, repairing what a backend may not report
		(see playback::makeSongTimeline()). The loop marker is derived from the *single* pass, so
		it stays correct for N >= 2 as well. */
	static void applyTimesTo (const playback::SongInfo& info, playback::SongTimeline& timeline,
							  int loopCount, double fadeSeconds);

	// --- the steps render() is made of ------------------------------------------------------
	// render() used to be one 220-line function: the block's timeline, the connectivity the host
	// reported, the two modes, the master bus stages, the output volume and the position/end
	// bookkeeping, all in one place. The steps below are that code, moved - same order, same
	// arithmetic, no allocation anywhere (review M-1).
	void publishBusConnectivity (const playback::OutputBusBuffers* buses, int busCount);
	static void clearBusBuffers (const playback::OutputBusBuffers* buses, int busCount, int numSamples);
	void reportSilenceWhenIdle (void);					// audio thread, lock-free
	void mixTracks (const playback::OutputBusBuffers* buses, int busCount, int usable);
	void renderPlainPlayer (int usable);
	/** The one-pole high pass behind the DC switch; see its definition for the arithmetic. */
	void applyDcBlock (const playback::OutputBusBuffers* buses, int busCount, int numSamples);

	void finaliseMasterBus (const playback::OutputBusBuffers* buses, int busCount, int usable);
	void applyOutputVolume (const playback::OutputBusBuffers* buses, int busCount, int numSamples);
	void publishPositionAndEnd (void);
	/** Follows the host clock at the top of a block, if the DAW sync is active and the mapping
		says the transport is off. Audio thread; takes the lock only when a correction is due. */
	void followDawTransport (void);

	// --- asynchronous loading (review M-1) ---
	// The worker only reads its own copied config and image, so it never needs the lock; the
	// install comes back to the message thread. `alive` is shared with the pending callbacks: the
	// destructor drops it before it stops the pool, so a result that arrives after the engine is
	// gone is dropped instead of writing into freed state.
	juce::ThreadPool loadPool { juce::ThreadPool::Options{}.withThreadName ("VGMToybox load")
									.withNumberOfThreads (1) };
	std::atomic<int> loadGeneration { 0 };
	std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

	mutable juce::CriticalSection lock;
	std::unique_ptr<playback::ISongInstance> song;		// the open song (backend object)
	std::vector<std::unique_ptr<playback::ITrackPlayer>> trackPlayers;
	std::vector<float> mixLeft, mixRight;				// per-track scratch for render()

	// A backend that renders every track in one pass: one interleaved stereo buffer per
	// track, the pointer array renderTracks() expects, and de-interleaved copies the mixing
	// loop below reads like any other track. Allocated when a song is installed, which
	// happens on the message thread, so the audio thread only writes into them.
	bool inPlaceMode = false;
	std::vector<float> inPlaceStereo;
	std::vector<float> inPlaceLeft, inPlaceRight;
	std::vector<float*> inPlaceBuffers;
	std::vector<float> masterLeft, masterRight;			// the internal master bus sum
	std::vector<std::unique_ptr<TrackState>> tracks;	// one per layout entry (UI state)
	std::vector<playback::TrackInfo> trackLayout;
	std::atomic<int> soloCount { 0 };
	std::atomic<int> masterMidiChannel { 0 };	// 0 = Off, 1..16
	std::atomic<int> midiResetGeneration { 0 };
	std::atomic<int> midiPitchBendRange { playback::defaultPitchBendRange };
	std::atomic<bool> midiPitchBendEnabled { true };
	const playback::MidiSequence* midiSequence = nullptr;
	std::size_t midiCursor = 0;
	int midiSeenGeneration = 0;
	int midiSeenBendRange = playback::defaultPitchBendRange;	// audio-side copy of the two settings:
	bool midiSeenBendEnabled = true;							// a change re-sends the RPN
	std::uint32_t midiSeenChannels = 0;							// channels the RPN was last sent to
	// Channels this engine has bent and not put back: a 0xE0 went out on them and nothing has
	// centred them since. A seek, a song swap and the "bends off" switch each retire exactly
	// these - the state this engine created, on the channels it created it on - never a channel
	// that was never bent.
	std::uint32_t midiBentChannels = 0;
	bool midiWasEnabled = false;
	// The notes this engine has sent a note-on for and not yet ended, per channel, 128 bits
	// each. A stop cannot trust All Notes Off to be honoured - several receivers ignore it, and
	// a MIDI monitor then keeps showing the note - while a note-off for a note the receiver was
	// told to start is the one message it has to act on. Audio thread only: emitMidiForBlock()
	// adds and removes, flushActiveNotesOff() (from the idle branch of render()) drains it.
	std::uint64_t midiActiveNotes[16][2] = {};
	double midiLastSongTime = -1.0;	// in the sequence's own (single pass) coordinate system
	bool mixerEnabled = true;	// the mixer card is always there - off is the CPU-saving opt-out
	// the routing scheme new songs start with (message thread only - the audio thread reads the
	// per-track targets, never this)
	playback::DefaultRouting defaultRouting = playback::DefaultRouting::master;

	juce::MemoryBlock fileData;			// the song image; the backend loads its players from it
	juce::File currentFile;
	std::vector<ChipEntry> chips;
	std::map<juce::String, juce::String> tags;

	double sampleRate = 44100.0;
	int maxBlockSize = 512;
	int loopCount = 1;
	double fadeSeconds = 3.0;
	// Two independent volume stages - render() applies the mixer master first and the output
	// volume last, so neither control is driven by the other.
	std::atomic<float> masterVolume { 1.0f };		// output volume (transport card slider)
	std::atomic<float> mixerMasterGain { 1.0f };
	std::atomic<float> sourceGain { 2.5119f };	// +8 dB, the NES default

	// The scope: a ring of the final mix. Twelve thousand samples is a quarter second at 48 kHz -
	// long enough to see a note's shape, short enough to stay a snapshot.
	static constexpr int kScopeSamples = 12000;
	std::array<std::atomic<float>, (std::size_t) kScopeSamples> scopeSamples {};
	std::atomic<std::uint32_t> scopeWrite { 0 };

	// The snapshot handover: written by the audio thread at the end of a block, read by the
	// message thread. Its own lock, never the engine's - see setStateCaptureWanted.
	mutable std::mutex stateMutex;
	playback::ChipStateSnapshot stateSnapshot;
	std::atomic<bool> stateCaptureWanted { false };




	void captureScope (const float* left, int numSamples);	// mixer master (master strip fader)

	// song facts for the lock-free getters - written on the message thread while the lock is
	// held (load / loop / fade / sample-rate changes), read without the lock
	playback::SongTimeline cachedTimeline;

	playback::DawSync dawSync;

	std::atomic<bool> loaded { false };
	std::atomic<bool> playing { false };
	std::atomic<bool> paused { false };
	std::atomic<bool> finished { false };
	std::atomic<double> positionSeconds { 0.0 };	// published by render(), read by the UI
	std::atomic<float> outputPeakL { 0.0f };		// master meter, published by render()
	std::atomic<float> outputPeakR { 0.0f };
	std::atomic<int> starvedBlocks { 0 };			// diagnostics - see getStarvedBlocks()

	// Seeking on a worker: a backend that isolates per track has to replay every one of them, and
	// an NSF has no state to jump into - so the work is seconds, and doing it on the message thread
	// freezes the window. The request is published here and the worker does the replay; the number
	// the UI draws is the request, which is the only thing it can know.
	std::atomic<int> playerCount { 0 };
	std::atomic<bool> seekRequested { false };
	std::atomic<bool> seekThreadShouldStop { false };
	std::atomic<double> seekTargetSeconds { 0.0 };
	std::thread seekThread;
	std::mutex seekMutex;
	std::condition_variable seekCv;

	// Output routing: the master bus target/mute and the connectivity the host showed us in the
	// last block (bit i = bus i was connected; bus 0 counts as connected until told otherwise).
	std::atomic<playback::OutputTarget> masterOutput { 0 };		// Out 1/2 by default
	std::atomic<bool> dcBlockEnabled { false };

	/** One filter state per bus and channel: a DC blocker is a one-pole high pass, and its memory
		must not be reset between blocks or every block boundary clicks. */
	struct DcBlockState
	{
		float lastInL = 0.0f, lastOutL = 0.0f;
		float lastInR = 0.0f, lastOutR = 0.0f;
	};

	std::array<DcBlockState, playback::kOutputBusCount> dcBlockStates {};

	std::atomic<bool> chipsMuteable { false };
	std::atomic<bool> midiEmittable { false };
	bool backendNeedsDcBlock = false;
	std::atomic<bool> masterMuted { false };
	std::atomic<std::uint64_t> connectedBuses { 1 };
	static_assert (playback::kOutputBusCount <= 64,
				   "more than 64 output buses need the connectivity mask to become an array");

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaybackEngine)
};

} // namespace toybox
