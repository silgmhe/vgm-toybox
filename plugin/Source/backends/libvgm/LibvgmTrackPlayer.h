#pragma once

#include <atomic>
#include <memory>
#include <utility>
#include <vector>

#include <juce_core/juce_core.h>

#include "stdtype.h"
#include "player/playera.hpp"
#include "utils/DataLoader.h"

#include "playback/SongBackend.h"

namespace toybox::backends::libvgm
{

/**
	One libvgm player instance - the adapter's ITrackPlayer.

	The engine creates one of these per mixer track (and a single un-isolated one in plain
	player mode). Every instance has its own DataLoader over the *shared* song image (one copy
	of the image, N player states) and its own mute configuration, so a track can suspend every
	device it does not need: a suspended device (`PLR_MUTE_OPTS::disable`) is not emulated at
	all and therefore costs no CPU.

	Except render() (audio thread, called while the engine holds its lock) every method is
	message-thread only.
*/
class LibvgmTrackPlayer final : public playback::ITrackPlayer
{
public:
	LibvgmTrackPlayer (void);
	~LibvgmTrackPlayer (void) override;

	/** Output format and scratch buffers - message thread, before load(). */
	void prepare (double hostSampleRate, int maxBlockSize);
	/** Loads a song image through a DataLoader over the shared memory. Message thread. */
	bool load (const playback::SongMemory& memory, juce::String& errorMessage);

	/** The track list of the song; setTrackAudible() looks the track up in it. */
	void setTrackList (const std::vector<playback::TrackInfo>* trackList) { tracks = trackList; }

	/** The songs's DPCM samples, taken once by the song instance that owns this player.

		Handed in rather than collected here: a VGM says what it starts in its command stream, so
		one walk answers for every player, and each of them walking it again would be the same
		answer at N times the cost.
	*/
	void setSamples (std::shared_ptr<const std::vector<playback::DecodedSample>> list)
	{
		sampleList = std::move (list);
	}

	// --- playback::ITrackPlayer ---------------------------------------------------
	void start (void) override;
	void pause (void) override;
	bool seekSeconds (double seconds) override;
	void setTimeline (int loopCount, double fadeSeconds) override;
	void setTrackAudible (playback::TrackId track, bool audible) override;
	void setChipAudible (playback::DeviceId device, bool audible) override;
	int render (float* left, float* right, int numSamples) override;
	double positionSeconds (void) override;
	bool isFinished (void) const override { return finished.load(); }

	/** The song's DPCM samples, as the song instance collected them. Empty for a file with no
		data blocks, which is a file that carries no sample bytes at all. */
	std::vector<playback::DecodedSample> samples (void) const override;

	/** Metadata access for the song instance that owns this player as its probe. */
	PlayerA& player (void) { return playerEngine; }

private:
	static uint8_t eventCallback (PlayerBase* player, void* userParam, uint8_t evtType, void* evtParam);
	uint8_t handleEvent (uint8_t evtType, void* evtParam);

	void unload (void);
	void applyMuting (uint32_t deviceId, const PLR_MUTE_OPTS& muteOpts);
	/** Rebuilds the whole mute configuration from the isolated track and the chip mute states. */
	void applyIsolation (void);
	bool isChipAudible (playback::DeviceId device) const;
	const playback::TrackInfo* findMyTrack (void) const;

	PlayerA playerEngine;
	DATA_LOADER* dataLoader = nullptr;
	std::vector<WAVE_32BS> renderBuffer;	// scratch buffer for the audio thread
	double sampleRate = 44100.0;
	int maxBlockSize = 512;
	int loopCount = 1;
	double fadeSeconds = 0.0;
	bool loopsForever = false;				// the timeline reports an infinite runtime
	bool isolated = false;					// setTrackAudible() named a track
	playback::TrackId myTrack {};
	bool myTrackAudible = true;
	std::vector<std::pair<playback::DeviceId, bool>> chipAudible;
	const std::vector<playback::TrackInfo>* tracks = nullptr;
	std::atomic<bool> finished { false };

	/** Empty until the song instance fills it; shared because every player of one song sees the
		same list and none of them owns it. */
	std::shared_ptr<const std::vector<playback::DecodedSample>> sampleList;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibvgmTrackPlayer)
};

} // namespace toybox::backends::libvgm
