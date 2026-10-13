#pragma once

#include <atomic>
#include <map>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

#include "stdtype.h"
#include "player/playera.hpp"
#include "utils/DataLoader.h"

namespace vgmplay
{

/** Metadata of one sound chip used by the current song. */
struct ChipEntry
{
	juce::String name;
	juce::String core;
	UINT32 devID = 0;			// PLR_DEV_ID() as reported by libvgm (for SetDeviceMuting)
	bool muted = false;			// muted by the user
};

/**
	Wrapper around libvgm's PlayerA.

	The message thread uses loadFile()/play()/pause()/stop()/setPositionSeconds() and reads the
	playback state, while the audio thread calls render(). All access to the libvgm player objects
	is serialised through one lock: the audio thread uses a *try*-lock (and outputs silence if the
	message thread is currently loading a file), the message thread uses a normal lock.
*/
class VgmEngine
{
public:
	VgmEngine();
	~VgmEngine();

	// --- message thread ---
	/** Loads a .vgm/.vgz file and starts playback. Returns false and fills errorMessage on failure. */
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
	void setMasterVolume (float linear);	// 1.0 = 100 %
	void setChipMuted (size_t chipIndex, bool shouldBeMuted);

	bool isLoaded (void) const { return loaded; }
	bool isPlaying (void) const { return playing.load(); }
	bool isFinished (void) const { return finished.load(); }
	double getPositionSeconds (void) const;
	double getDurationSeconds (void) const;
	/** Position of the loop point inside a single playthrough, or a negative value if the song
		has no loop point (or nothing is loaded). */
	double getLoopPointSeconds (void) const;
	int getLoopCount (void) const { return loopCount; }
	double getFadeSeconds (void) const { return fadeSeconds; }
	float getMasterVolume (void) const { return masterVolume; }
	const juce::File& getFile (void) const { return currentFile; }
	juce::String getTag (const juce::String& tagName) const;
	const std::vector<ChipEntry>& getChips (void) const { return chips; }

	// --- audio thread ---
	void render (float* const* outputs, int numSamples);

private:
	static UINT8 eventCallback (PlayerBase* player, void* userParam, UINT8 evtType, void* evtParam);
	UINT8 handleEvent (UINT8 evtType, void* evtParam);

	void applyOutputSettings (void);
	double getPlayLengthSeconds (void) const;	// caller must hold the lock
	void updatePlayerConfig (void);
	void enumerateTags (void);
	void enumerateChips (void);
	void resetState (void);

	mutable juce::CriticalSection lock;
	PlayerA player;
	DATA_LOADER* dataLoader = NULL;
	juce::MemoryBlock fileData;			// keeps the file/image alive while loaded
	juce::File currentFile;

	std::vector<WAVE_32BS> renderBuffer;	// scratch buffer for the audio thread
	std::vector<ChipEntry> chips;
	std::map<juce::String, juce::String> tags;

	double sampleRate = 44100.0;
	int maxBlockSize = 512;
	int loopCount = 1;
	double fadeSeconds = 3.0;
	float masterVolume = 1.0f;

	std::atomic<bool> loaded { false };
	std::atomic<bool> playing { false };
	std::atomic<bool> finished { false };

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VgmEngine)
};

} // namespace vgmplay
