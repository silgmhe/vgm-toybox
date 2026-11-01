#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <juce_core/juce_core.h>

#include "stdtype.h"
#include "player/playera.hpp"
#include "utils/DataLoader.h"

#include "playback/SongBackend.h"

#include "VgmReplayer.h"

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

	/** The parsed song and the devices it started, for the display.

		Handed in rather than parsed here, for the same reason `setSamples` is: one parse answers for
		every player of the song, and each of them walking it again would be the same answer at N
		times the cost. A null song leaves `captureState` returning false, which is what the panel
		reads as "this backend has nothing to say".
	*/
	void setReplaySource (std::shared_ptr<const midi::SongIr> ir,
						  const std::vector<PLR_DEV_INFO>& devices);

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

	/** What the chips are doing, for the keyboard page.

		Audio thread, called after render(): the replayer is advanced to the position render() just
		reached, and its decoders are read. Nothing is locked - the replayer is this player's alone,
		and `captureState` is the only thing that touches it.
	*/
	bool captureState (playback::ChipStateSnapshot& snapshot) const override;

	/** The song's DPCM samples, as the song instance collected them. Empty for a file with no
		data blocks, which is a file that carries no sample bytes at all. */
	std::vector<playback::DecodedSample> samples (void) const override;

	/** Metadata access for the song instance that owns this player as its probe. */
	PlayerA& player (void) { return playerEngine; }

private:
	static uint8_t eventCallback (PlayerBase* player, void* userParam, uint8_t evtType, void* evtParam);
	uint8_t handleEvent (uint8_t evtType, void* evtParam);

	void unload (void);

	/** The engine track a device channel belongs to, or -1.

		The panel offers mute and solo on a row only when this says where to send them, and the
		engine owns that state - a second copy here would drift from the mixer's.
	*/
	int trackFor (uint32_t deviceId, int channel) const;
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

	// --- the display path ---------------------------------------------------------
	// All of it `mutable` because captureState is const by contract and has to advance the replayer
	// to the position render() reached. Nothing here is shared with another thread: the engine calls
	// captureState on the audio thread, after render, and this player owns the state.

	std::shared_ptr<const midi::SongIr> replayIr;
	std::vector<PLR_DEV_INFO> replayDevices;
	mutable std::unique_ptr<VgmReplayer> replayer;

	/** What the display draws for each DPCM sample: the bytes the export collected, and the rate it
		declares.

		Cached by sample identity because the row asks sixty times a second and the list holds a few
		tens of kilobytes of levels - and because the two must not be taken from different sources.
		The rate here is the one an exported WAV of the same sample would carry.
	*/
	struct DpcmLookup
	{
		std::shared_ptr<const std::vector<float>> wave;
		float rate = 0.0f;
	};

	/** A hit that happened between two snapshots.

		The capture samples the chip once per audio block and skips a block outright when the panel
		is reading the previous snapshot - which the engine's own comment calls free, and is free for
		a sustaining note. A drum hit lasts five to twenty milliseconds, so one skipped block can
		hold the whole hit and the row never lights.
	*/
	struct PendingHit
	{
		int note = -1;
		int blocksLeft = 0;
	};

	/** Everything this player remembers about samples between frames.

		Four tables that only ever make sense together: which note a sample identity is given, the
		levels behind it, the hits that no snapshot has carried yet, and the counts those hits were
		noticed from. Kept in one place so that the player reads as "replay plus a ledger" rather
		than as a class with five maps of its own - the review's problem 8, first step.
	*/
	struct Ledger
	{
		/** Which note each sample is given, keyed by the chip's own sample identity.

			Built once from the whole song, because the mapping is a property of the song rather than
			of a moment: a drum has no pitch, so the only way to tell one hit from another on a note
			grid is to give each sample a note of its own. Numbered from middle C upwards in the order
			the samples first play.
		*/
		std::map<uint32_t, int> notes;

		/** Decoded levels per identity, filled on first use and shared from then on. */
		mutable std::map<uint32_t, DpcmLookup> samples;

		/** Keyed by device and channel. Kept for a few blocks after the hit so at least one
			snapshot carries it; how long it then stays visible is the panel's decision. */
		mutable std::map<uint32_t, PendingHit> pending;

		/** The last hit count seen per channel, for noticing that a new one happened. */
		mutable std::map<uint32_t, uint32_t> lastHits;

		/** Fills `notes` from the whole song: one note per distinct sample, from middle C up.

			A property of the song rather than of a moment, which is why it is built once at load
			rather than per frame - a drum has no pitch, so "which sample" is the only thing a note
			grid can be told, and that answer needs the whole file.
		*/
		void buildNotes (const midi::SongIr& ir);

		/** The levels behind one identity, decoded on first use from the export's collection.

			`nullptr` when the collector never saw this sample - a file that carried registers but
			not the memory they point at. The row then keeps its rate and draws no wave, which is the
			truth about that hit rather than a placeholder that looks like a sound.
		*/
		const DpcmLookup* lookup (uint32_t sampleId,
								  const std::vector<playback::DecodedSample>* collection) const;
	};

	mutable Ledger ledger;

	/** Set by a seek or a loop point, consumed by the next captureState. A counter rather than a
		flag: two seeks between two frames must not become one rewind and a position that never
		arrives. */
	mutable std::atomic<bool> pendingRewind { false };

	/** Where render() left the song, in seconds. Written on the audio thread in render(),
		read in captureState() - which is the same thread, so this is for the compiler rather than
		for a race. */
	mutable std::atomic<double> lastPosition { 0.0 };

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibvgmTrackPlayer)
};

} // namespace toybox::backends::libvgm
