#pragma once

#include <atomic>
#include <vector>

#include <juce_core/juce_core.h>

#include "stdtype.h"
#include "player/playera.hpp"
#include "utils/DataLoader.h"

namespace vgmplay
{

/**
	One playback track: a libvgm player instance whose output carries a single chip channel.

	MixerEngine creates one of these per track in mixer mode, and a single un-isolated one in
	plain player mode. Every instance has its own DataLoader over the *shared* file memory (one
	copy of the song image, N player states) and its own mute configuration, so a mixer track can
	suspend every device it does not need - a suspended device (`PLR_MUTE_OPTS::disable`) is not
	emulated at all and therefore costs no CPU.

	Except for render() (audio thread, called while the engine holds its lock) every method is
	message-thread only.
*/
class TrackPlayer
{
public:
	TrackPlayer (void);
	~TrackPlayer (void);

	/** Sets the output format and allocates the scratch buffers - call before loadFile(). */
	void prepare (double newSampleRate, int newMaxBlockSize);

	/** Loads a file image from the shared memory block (the block must outlive the player). */
	bool loadFile (const juce::MemoryBlock& fileData, juce::String& errorMessage);
	void unload (void);

	void start (void);
	void seekToSample (UINT32 sample);		// also clears the finished flag
	void applyConfig (const PlayerA::Config& config);
	void applyMuting (UINT32 deviceID, const PLR_MUTE_OPTS& muteOpts);

	/** Renders one block into the internal stereo float buffers (audio thread). */
	void render (int numSamples);
	const float* getLeft (void) const { return left.data(); }
	const float* getRight (void) const { return right.data(); }
	int getRenderedSamples (void) const { return renderedSamples; }

	/** Set from the player's event callback - the audio thread writes it. */
	bool hasFinished (void) const { return finished.load(); }
	void clearFinished (void) { finished = false; }

	/** The underlying player: song metadata and position queries of the engine. */
	PlayerA& player (void) { return playerEngine; }

private:
	static UINT8 eventCallback (PlayerBase* player, void* userParam, UINT8 evtType, void* evtParam);
	UINT8 handleEvent (UINT8 evtType, void* evtParam);

	PlayerA playerEngine;
	DATA_LOADER* dataLoader = nullptr;
	std::vector<WAVE_32BS> renderBuffer;	// scratch buffer for the audio thread
	std::vector<float> left, right;			// the rendered block as float stereo
	double sampleRate = 44100.0;
	int maxBlockSize = 512;
	int renderedSamples = 0;
	std::atomic<bool> finished { false };

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackPlayer)
};

} // namespace vgmplay
