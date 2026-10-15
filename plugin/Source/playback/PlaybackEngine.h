#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <vector>

#include <juce_core/juce_core.h>

#include "playback/SongBackend.h"

namespace vgmplay
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
	this class, the UI or the host integration: see docs/playback-backend-design.md.

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
	/** Sets the output sample rate / maximum block size (host sample rate). */
	void prepare (double newSampleRate, int maxBlockSize);

	void play (void);
	void pause (void);
	void stop (void);
	void setPositionSeconds (double seconds);
	void setLoopCount (int loops);			// 0 = loop forever
	void setFadeSeconds (double seconds);
	/** The output volume: the **last** gain stage of the engine, applied after every mixer
		track and after the mixer's own master gain. Lock-free (one atomic). */
	void setMasterVolume (float linear);	// 1.0 = 100 %
	/** The mixer's master gain - the master strip's fader. It scales the sum of the tracks
		and sits *before* the output volume, so both controls are independent stages. */
	void setMixerMasterGain (float linear);	// 1.0 = 100 %
	void setChipMuted (size_t chipIndex, bool shouldBeMuted);

	bool isLoaded (void) const { return loaded; }
	bool isPlaying (void) const { return playing.load(); }
	bool isFinished (void) const { return finished.load(); }
	/** Lock-free snapshot of the playback position: render() publishes it once per block and
		every seek updates it, so the UI polling never contends for the render lock. */
	double getPositionSeconds (void) const { return positionSeconds.load(); }
	/** Song length, cached while the lock is held when the file, the loop count, the fade time
		or the sample rate changes - read without the lock. */
	double getDurationSeconds (void) const { return cachedDurationSeconds; }
	/** Position of the loop point inside a single playthrough, or a negative value if the song
		has no loop point (or nothing is loaded). */
	double getLoopPointSeconds (void) const { return cachedLoopPointSeconds; }
	int getLoopCount (void) const { return loopCount; }
	double getFadeSeconds (void) const { return fadeSeconds; }
	float getMasterVolume (void) const { return masterVolume.load(); }
	float getMixerMasterGain (void) const { return mixerMasterGain.load(); }
	const juce::File& getFile (void) const { return currentFile; }
	juce::String getTag (const juce::String& tagName) const;
	const std::vector<ChipEntry>& getChips (void) const { return chips; }

	// --- mixer ---
	/** Mixer mode renders one isolated track per channel; off means the plain single player. */
	bool isMixerEnabled (void) const { return mixerEnabled; }
	/** Rebuilds the players for the other mode, keeping the position and the playing state. */
	void setMixerEnabled (bool shouldBeEnabled);

	/** One entry per track the backend reports - the list exists in both modes. */
	const std::vector<playback::TrackInfo>& getTracks (void) const { return trackLayout; }

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

	// --- audio thread ---
	void render (float* const* outputs, int numSamples);

private:
	struct TrackState
	{
		std::atomic<float> gain { 1.0f };
		std::atomic<float> pan { 0.0f };
		std::atomic<bool> mute { false };
		std::atomic<bool> solo { false };
		std::atomic<float> peakL { 0.0f };
		std::atomic<float> peakR { 0.0f };
	};

	double getPlayLengthSeconds (void) const;			// caller must hold the lock
	void refreshCachedTimes (void);						// caller must hold the lock
	void updatePlayerTimeline (void);					// caller must hold the lock
	void enumerateTags (void);
	void enumerateChips (void);
	void applyTrackIsolation (void);					// caller must hold the lock
	void applyChipMutes (void);							// caller must hold the lock
	bool createPlayers (juce::String& errorMessage);	// caller must hold the lock
	void destroyPlayers (void);
	void rebuildPlayers (juce::String& errorMessage);	// caller must hold the lock
	void resetState (void);

	mutable juce::CriticalSection lock;
	std::unique_ptr<playback::ISongInstance> song;		// the open song (backend object)
	std::vector<std::unique_ptr<playback::ITrackPlayer>> trackPlayers;
	std::vector<float> mixLeft, mixRight;				// per-track scratch for render()
	std::vector<std::unique_ptr<TrackState>> tracks;	// one per layout entry (UI state)
	std::vector<playback::TrackInfo> trackLayout;
	std::atomic<int> soloCount { 0 };
	bool mixerEnabled = true;	// the mixer card is always there - off is the CPU-saving opt-out

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
	std::atomic<float> mixerMasterGain { 1.0f };	// mixer master (master strip fader)

	// song facts for the lock-free getters - written on the message thread while the lock is
	// held (load / loop / fade / sample-rate changes), read without the lock
	double cachedDurationSeconds = 0.0;
	double cachedLoopPointSeconds = -1.0;
	bool loopsForever = false;

	std::atomic<bool> loaded { false };
	std::atomic<bool> playing { false };
	std::atomic<bool> finished { false };
	std::atomic<double> positionSeconds { 0.0 };	// published by render(), read by the UI
	std::atomic<float> outputPeakL { 0.0f };		// master meter, published by render()
	std::atomic<float> outputPeakR { 0.0f };

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlaybackEngine)
};

} // namespace vgmplay
