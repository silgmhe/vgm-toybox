#include "PlaybackEngine.h"

#include <juce_audio_basics/juce_audio_basics.h>
// for MessageManager::callAsync(): the install half of an asynchronous load has to run on the
// message thread (review M-1). juce_events is a dependency of every target that compiles this
// file, including the headless core tests, which never call the async path.
#include <juce_events/juce_events.h>

#include "playback/BackendRegistry.h"
#include "playback/SessionState.h"

namespace toybox
{

PlaybackEngine::PlaybackEngine (void)
{
}

PlaybackEngine::~PlaybackEngine (void)
{
	// A result that is already queued on the message loop must see this before it touches the
	// engine, and the worker has to be gone before the lock (and the object) does: the pool is
	// stopped here, and the job itself only touches its own copies, so interrupting it is safe.
	// The timeout keeps a pathological build from hanging the host on teardown.
	alive->store (false);
	loadPool.removeAllJobs (true, 3000);

	const juce::ScopedLock sl (lock);
	resetState();
}

//==============================================================================
// message thread

void PlaybackEngine::prepare (double newSampleRate, int maxBlock)
{
	bool needsRebuild = false;

	{
		const juce::ScopedLock sl (lock);

		const double newRate = (newSampleRate > 0.0) ? newSampleRate : 44100.0;
		const int newBlockSize = juce::jmax (1, maxBlock);
		const bool settingsChanged = (newRate != sampleRate) || (newBlockSize != maxBlockSize);

		sampleRate = newRate;
		maxBlockSize = newBlockSize;

		// the scratch the mix renders each track into (allocated here, never on the audio thread)
		mixLeft.assign ((size_t) maxBlockSize, 0.0f);
		mixRight.assign ((size_t) maxBlockSize, 0.0f);
		// the internal master bus: tracks routed to `Master` accumulate here before it is handed to
		// the master's own target bus
		masterLeft.assign ((size_t) maxBlockSize, 0.0f);
		masterRight.assign ((size_t) maxBlockSize, 0.0f);

		needsRebuild = loaded.load() && settingsChanged;
	}

	if (! needsRebuild)
	{
		return;
	}

	// The sound cores are created at the output sample rate, so a loaded song has to be re-created
	// when it changes - otherwise the emulation would keep running at the old rate (wrong pitch and
	// playback speed). rebuildPlayers() builds outside the lock and only installs under it, so a
	// sample-rate change does not silence the audio thread for the whole rebuild.
	juce::String errorMessage;
	rebuildPlayers (errorMessage);
	if (! errorMessage.isEmpty())
	{
		juce::Logger::writeToLog ("[VGMToybox] reload after sample rate change failed: " + errorMessage);
	}
}

bool PlaybackEngine::loadFile (const juce::File& file, juce::String& errorMessage)
{
	// Reading the file is the one part of a load whose cost is unbounded - a 30 MB VGM from a cold
	// cache, or a file something else is still writing - and it touches nothing this engine owns,
	// so the audio thread keeps rendering through it instead of falling silent for as long as the
	// disk takes (the review's H-1).
	const bool exists = file.existsAsFile();
	juce::MemoryBlock data;
	const bool readable = exists && file.loadFileAsData (data) && data.getSize() > 0;

	if (! readable)
	{
		const juce::ScopedLock sl (lock);
		resetState();	// stop playback and release the previous file first
		errorMessage = exists ? "Could not read the file." : "The file does not exist.";
		return false;
	}

	return loadImage (std::move (data), file, true, errorMessage);
}

bool PlaybackEngine::loadImage (juce::MemoryBlock image, const juce::File& file, bool startPlaying,
								juce::String& errorMessage)
{
	// This is a request of its own: a load that is still building on the worker must not install
	// itself over it.
	++loadGeneration;

	// Outside the lock: unpacking and starting the sound cores, into a song of its own. The
	// current song keeps playing while this happens - a failed load still stops playback in
	// installLoaded(), exactly as it always has. The configuration is copied first so the build
	// never reads a member render() can see - the same copy loadFileAsync() hands to its worker.
	const BuildConfig config = makeBuildConfig();
	LoadedSong built = buildSong (config, image, file, errorMessage);

	return installLoaded (std::move (built), std::move (image), file, startPlaying);
}

bool PlaybackEngine::installLoaded (LoadedSong&& built, juce::MemoryBlock&& image,
									const juce::File& file, bool startPlaying)
{
	// Under the lock: the swap, and nothing else.
	const juce::ScopedLock sl (lock);

	resetState();

	if (built.song == nullptr)
	{
		return false;	// errorMessage was filled in by the backend
	}

	// the backend unpacks a compressed container (.vgz) itself - the image stays as it was read
	fileData = std::move (image);
	currentFile = file;

	installSong (std::move (built));

	positionSeconds = 0.0;
	finished = false;
	playing = startPlaying;

	return true;
}

void PlaybackEngine::loadFileAsync (const juce::File& file, LoadCallback onDone, bool startPlaying)
{
	// Snapshot on the message thread: from here on the job only touches its own copies, so a
	// sample-rate change, a mode switch or a close on the message thread cannot race the build.
	const BuildConfig config = makeBuildConfig();
	const int generation = ++loadGeneration;
	auto* const engine = this;
	const auto lifetime = alive;

	loadPool.addJob ([engine, lifetime, config, file, generation, startPlaying,
					  onDone = std::move (onDone)] () mutable
	{
		juce::MemoryBlock data;
		juce::String error;
		bool ok = file.existsAsFile();

		if (! ok)
		{
			error = "The file does not exist.";
		}
		else if (! file.loadFileAsData (data) || data.getSize() == 0)
		{
			ok = false;
			error = "Could not read the file.";
		}

		LoadedSong built;
		if (ok)
		{
			built = buildSong (config, data, file, error);
			ok = built.song != nullptr;
		}

		// Back to the message thread for the swap, where every other mutator lives and where the
		// callback that touches the window has to run.
		juce::MessageManager::getInstance()->callAsync (
			[engine, lifetime, generation, built = std::move (built), data = std::move (data),
			 file, ok, error, startPlaying, onDone = std::move (onDone)] () mutable
			{
				if (! lifetime->load())
				{
					return;		// the engine is gone; the result is dropped
				}

				engine->finishLoad (generation, std::move (built), std::move (data), file, ok, error,
									startPlaying, onDone);
			});
	});
}

void PlaybackEngine::finishLoad (int generation, LoadedSong&& built, juce::MemoryBlock&& image,
								 const juce::File& file, bool ok, const juce::String& errorMessage,
								 bool startPlaying, const LoadCallback& onDone)
{
	// A newer request (or unloadFile()) bumped the generation while this one was building: it owns
	// the engine now, and this result is dropped without reporting - a stale failure must not
	// paint an error over a song that loaded fine, and a stale success must not replace it.
	if (generation != loadGeneration.load())
	{
		return;
	}

	bool installed = false;

	if (ok)
	{
		installed = installLoaded (std::move (built), std::move (image), file, startPlaying);
	}
	else
	{
		const juce::ScopedLock sl (lock);
		resetState();	// a failed load stops playback and releases the previous song, as always
	}

	if (onDone != nullptr)
	{
		onDone (installed, errorMessage);
	}
}

PlaybackEngine::BuildConfig PlaybackEngine::makeBuildConfig (void) const
{
	BuildConfig config;
	config.sampleRate = sampleRate;
	config.maxBlockSize = maxBlockSize;
	config.loopCount = loopCount;
	config.fadeSeconds = fadeSeconds;
	config.mixerEnabled = mixerEnabled;
	config.defaultRouting = defaultRouting;
	config.chips = chips;
	return config;
}

//==============================================================================
// project state

void PlaybackEngine::writeState (juce::MemoryBlock& destData) const
{
	// Everything is copied out under the lock and encoded after it: the snapshot is bounded work
	// (worst case a 16 MB image), the base64 plus gzip pass is not, and a host autosaving used to
	// be the reason this must not hold the lock for half a second.
	//
	// The transport is not in here on purpose: whether the song was playing when the project was
	// saved is an accident of when the host autosaved, so a project always opens stopped (and
	// readState() installs that state with the song instead of starting it).
	playback::SessionState state;
	{
		const juce::ScopedLock sl (lock);

		state.loopCount = loopCount;
		state.fadeSeconds = fadeSeconds;
		state.volume = masterVolume.load();
		state.syncEnabled = dawSync.isEnabled();
		state.syncCueSeconds = dawSync.getCueSeconds();
		state.syncCueOffsetMs = dawSync.getCueOffsetMs();
		state.syncKnobRangeMs = dawSync.getKnobRangeMs();
		state.mixerEnabled = mixerEnabled;
		state.mixerMasterGain = mixerMasterGain.load();
		state.masterMuted = masterMuted.load();
		state.masterOutput = masterOutput.load();
		state.masterMidiChannel = masterMidiChannel.load();
		state.defaultRouting = defaultRouting;

		// The name, not the path: a project has to open where the file never existed, and the
		// extension is the one part of the name that still does something (it picks the backend).
		state.songName = currentFile.getFileName();
		state.songImage = fileData;

		state.tracks.reserve (tracks.size());
		for (size_t i = 0; i < tracks.size(); ++i)
		{
			playback::SessionTrackState track;
			track.index = i;
			track.gain = tracks[i]->gain.load();
			track.pan = tracks[i]->pan.load();
			track.mute = tracks[i]->mute.load();
			track.solo = tracks[i]->solo.load();
			track.output = tracks[i]->output.load();
			track.midiChannel = tracks[i]->midiChannel.load();
			state.tracks.push_back (track);
		}
	}

	destData = playback::encodeSessionState (state);
}

bool PlaybackEngine::readState (const void* data, int sizeInBytes, juce::String& errorMessage)
{
	playback::SessionState state;
	if (! playback::decodeSessionState (data, sizeInBytes, state, errorMessage))
	{
		return false;
	}

	const bool hasSong = state.songImage.getSize() > 0;

	// The cue is a position on the *host's* timeline, so it survives a save/reopen unchanged.
	// The range goes in before the offset, otherwise a remembered offset would be clamped against
	// the default range first. A project written while the cue could be cleared stored -1 there;
	// clamping it to 0 - the DAW's start - is exactly what that state meant, and doing it here
	// rather than relying on setCueSeconds() ignoring the value keeps a previously loaded cue from
	// surviving the load.
	dawSync.setEnabled (state.syncEnabled);
	dawSync.setKnobRangeMs (state.syncKnobRangeMs);
	dawSync.setCueOffsetMs (state.syncCueOffsetMs);
	dawSync.setCueSeconds (juce::jmax (0.0, state.syncCueSeconds));

	if (hasSong)
	{
		// The values buildSong() consults have to be in place *before* the load: the mixer mode
		// decides whether one player per track is created, the routing scheme where the new
		// tracks go. They are set on the fields rather than through the public setters because
		// those would rebuild the song that is being replaced.
		{
			const juce::ScopedLock sl (lock);
			loopCount = state.loopCount;
			fadeSeconds = state.fadeSeconds;
			mixerEnabled = state.mixerEnabled;
			defaultRouting = state.defaultRouting;
		}

		setMixerMasterGain (state.mixerMasterGain);
		setMasterVolume (state.volume);
		setMasterMuted (state.masterMuted);
		setMasterOutput ((playback::OutputTarget) state.masterOutput);
		setMasterMidiChannel (state.masterMidiChannel);

		// Restored stopped, deliberately: a host opening a project must not make sound on its own
		// (the transport is not part of the state either - see writeState()).
		if (! loadImage (state.songImage, juce::File (state.songName), false, errorMessage))
		{
			return false;
		}
	}
	else
	{
		// No song rides along (nothing was loaded when this was saved, or it was over the
		// embedding limit). The rest of the state still restores, and a song that happens to be
		// loaded stays consistent with it.
		setLoopCount (state.loopCount);
		setFadeSeconds (state.fadeSeconds);
		setMixerEnabled (state.mixerEnabled);
		setDefaultRouting (state.defaultRouting);
		setMixerMasterGain (state.mixerMasterGain);
		setMasterVolume (state.volume);
		setMasterMuted (state.masterMuted);
		setMasterOutput ((playback::OutputTarget) state.masterOutput);
		setMasterMidiChannel (state.masterMidiChannel);
	}

	// Only now: installSong() replaced the track list, and how many tracks there are is a fact of
	// the song that was just built. Values for tracks it does not have are dropped - the project
	// may have been saved with a different song loaded.
	const size_t trackCount = getTracks().size();
	for (const auto& track : state.tracks)
	{
		if (track.index >= trackCount)
		{
			continue;
		}

		setTrackGain (track.index, track.gain);
		setTrackPan (track.index, track.pan);
		setTrackMute (track.index, track.mute);
		setTrackSolo (track.index, track.solo);
		setTrackOutput (track.index, track.output);
		setTrackMidiChannel (track.index, track.midiChannel);
	}

	if (! hasSong && state.songName.isNotEmpty() && ! state.songEmbedded)
	{
		errorMessage = "The song is too large to save with the project - load it again.";
		return false;
	}

	return true;
}

juce::MemoryBlock PlaybackEngine::getSongImage (void) const
{
	const juce::ScopedLock sl (lock);
	return fileData;
}

void PlaybackEngine::unloadFile (void)
{
	// A load that is still building must not install itself into the file that is being closed.
	++loadGeneration;

	const juce::ScopedLock sl (lock);
	resetState();
}

void PlaybackEngine::setMixerEnabled (bool shouldBeEnabled)
{
	{
		const juce::ScopedLock sl (lock);
		if (shouldBeEnabled == mixerEnabled)
		{
			return;
		}

		mixerEnabled = shouldBeEnabled;
		midiResetGeneration.fetch_add (1);
		if (! loaded.load())
		{
			return;
		}
	}

	// rebuild the players for the new mode; the track settings stay, they belong to the layout, and
	// rebuildPlayers() carries them over. The lock is not held across the rebuild - only the install
	// needs it - so switching the mixer does not stall the audio thread.
	juce::String errorMessage;
	rebuildPlayers (errorMessage);
	if (! errorMessage.isEmpty())
	{
		juce::Logger::writeToLog ("[VGMToybox] switching the mixer failed: " + errorMessage);
	}
}

void PlaybackEngine::play (void)
{
	if (! loaded.load())
	{
		return;
	}
	if (finished.load())
	{
		setPositionSeconds (0.0);	// restart a finished song
	}
	playing = true;
	paused = false;
	midiResetGeneration.fetch_add (1);
}

void PlaybackEngine::pause (void)
{
	playing = false;
	paused = true;
	midiResetGeneration.fetch_add (1);
}

void PlaybackEngine::stop (void)
{
	playing = false;
	paused = false;
	finished = false;
	midiResetGeneration.fetch_add (1);

	const juce::ScopedLock sl (lock);
	for (auto& player : trackPlayers)
	{
		player->seekSeconds (0.0);
	}
	positionSeconds = 0.0;
}

void PlaybackEngine::setPositionSeconds (double seconds)
{
	const juce::ScopedLock sl (lock);
	setPositionSecondsLocked (seconds);
}

void PlaybackEngine::followDawTransport (void)
{
	// The sync switch and the loaded flag are atomics, so a block that does not follow costs
	// nothing. Everything past this point - the players, the cached timeline - belongs to the
	// lock, which is the rule render() follows below: the message thread swaps the song and
	// rewrites the timeline, and the timeline is a handful of doubles that have to come from one
	// song rather than from two. The price is one try-lock per block while the host drives (~20 ns
	// against 10 ms of audio); a block that loses it is simply retried, since the mapping is
	// absolute.
	if (! dawSync.isActive() || ! loaded.load())
	{
		return;
	}

	const juce::ScopedTryLock syncTry (lock);
	if (! syncTry.isLocked() || trackPlayers.empty())
	{
		return;
	}

	// The decision itself is pure arithmetic over atomics and the snapshot taken above.
	const auto decision = dawSync.decide (cachedTimeline, positionSeconds.load(), playing.load());
	if (! decision.reposition)
	{
		return;
	}

	// The transport state changes *before* the seek, because a backend's start() may mean "begin at
	// the beginning" rather than "resume" - libvgm's does (VGMPlayer::Start() calls Reset()), so
	// starting after the seek would undo it. Seeking last is what makes the position the final
	// word. The other order was audible: the first block after the host started played from the top
	// of the song, and the next block seeked again to where it should have been.
	const bool engineWasPlaying = playing.load();

	if (decision.play)
	{
		if (! engineWasPlaying)
		{
			for (auto& player : trackPlayers)
			{
				player->start();
			}
		}
	}
	else
	{
		for (auto& player : trackPlayers)
		{
			player->pause();
		}
	}

	// A seek is the expensive part, which is why the mapping only asks for one when the engine is
	// more than 30 ms off, or when the transport state has to change at all.
	setPositionSecondsLocked (decision.position);

	playing = decision.play;
	paused = false;	// the host owns the transport; the local pause light must not survive it
	finished = false;
}

void PlaybackEngine::setPositionSecondsLocked (double seconds)
{
	if (trackPlayers.empty())
	{
		return;
	}

	const double duration = getPlayLengthSeconds();
	const double target = juce::jlimit (0.0, juce::jmax (0.0, duration), seconds);

	// every track seeks to the very same sample, which is what keeps them in sync
	for (auto& player : trackPlayers)
	{
		player->seekSeconds (target);
	}

	positionSeconds = target;
	finished = false;
	midiResetGeneration.fetch_add (1);
}

void PlaybackEngine::setLoopCount (int loops)
{
	const juce::ScopedLock sl (lock);
	loopCount = loops;
	updatePlayerTimeline();
	refreshCachedTimes();	// the cached duration includes the loop repetitions
}

void PlaybackEngine::setFadeSeconds (double seconds)
{
	const juce::ScopedLock sl (lock);
	fadeSeconds = seconds;
	updatePlayerTimeline();
	refreshCachedTimes();	// the cached duration includes the fade time
}

void PlaybackEngine::setMasterVolume (float linear)
{
	// The output volume is applied by render() itself (the last gain stage), so no player has
	// to be touched and no lock is needed.
	masterVolume.store (juce::jmax (0.0f, linear));
}

void PlaybackEngine::setMixerMasterGain (float linear)
{
	// the mixer master is a mix-time gain like the track gains: one atomic, read by render()
	mixerMasterGain.store (juce::jmax (0.0f, linear));
}

void PlaybackEngine::setChipMuted (size_t chipIndex, bool shouldBeMuted)
{
	const juce::ScopedLock sl (lock);
	if (chipIndex >= chips.size())
	{
		return;
	}

	ChipEntry& chip = chips[chipIndex];
	chip.muted = shouldBeMuted;

	if (trackPlayers.empty())
	{
		return;
	}

	// The players only need to know which devices are audible - the backend decides what that
	// means for its own state (libvgm suspends the device, which also saves the CPU).
	applyChipMutes();
}

//==============================================================================
// mixer parameters (plain atomics - the audio thread reads them without the lock)

void PlaybackEngine::setTrackGain (size_t trackIndex, float linearGain)
{
	if (trackIndex < tracks.size())
	{
		tracks[trackIndex]->gain.store (juce::jmax (0.0f, linearGain));
	}
}

float PlaybackEngine::getTrackGain (size_t trackIndex) const
{
	return (trackIndex < tracks.size()) ? tracks[trackIndex]->gain.load() : 0.0f;
}

void PlaybackEngine::setTrackPan (size_t trackIndex, float pan)
{
	if (trackIndex < tracks.size())
	{
		tracks[trackIndex]->pan.store (juce::jlimit (-1.0f, 1.0f, pan));
	}
}

float PlaybackEngine::getTrackPan (size_t trackIndex) const
{
	return (trackIndex < tracks.size()) ? tracks[trackIndex]->pan.load() : 0.0f;
}

void PlaybackEngine::setTrackMute (size_t trackIndex, bool shouldBeMuted)
{
	if (trackIndex < tracks.size())
	{
		tracks[trackIndex]->mute.store (shouldBeMuted);
		midiResetGeneration.fetch_add (1);
	}
}

bool PlaybackEngine::isTrackMuted (size_t trackIndex) const
{
	return (trackIndex < tracks.size()) && tracks[trackIndex]->mute.load();
}

void PlaybackEngine::setTrackSolo (size_t trackIndex, bool shouldBeSoloed)
{
	if (trackIndex >= tracks.size())
	{
		return;
	}

	TrackState& state = *tracks[trackIndex];
	if (state.solo.load() == shouldBeSoloed)
	{
		return;
	}

	state.solo.store (shouldBeSoloed);
	soloCount.fetch_add (shouldBeSoloed ? 1 : -1);
	midiResetGeneration.fetch_add (1);
}

bool PlaybackEngine::isTrackSoloed (size_t trackIndex) const
{
	return (trackIndex < tracks.size()) && tracks[trackIndex]->solo.load();
}

float PlaybackEngine::getTrackPeak (size_t trackIndex, int channel) const
{
	if (trackIndex >= tracks.size())
	{
		return 0.0f;
	}

	const TrackState& state = *tracks[trackIndex];
	return (channel == 1) ? state.peakR.load() : state.peakL.load();
}

float PlaybackEngine::getOutputPeak (int channel) const
{
	return (channel == 1) ? outputPeakR.load() : outputPeakL.load();
}

double PlaybackEngine::getTrackPositionSeconds (size_t trackIndex) const
{
	const juce::ScopedLock sl (lock);
	if (trackIndex >= trackPlayers.size())
	{
		return -1.0;
	}

	return trackPlayers[trackIndex]->positionSeconds();
}

//==============================================================================
// meters

void PlaybackEngine::clearTrackPeaks (void)
{
	// plain atomics, so this needs no lock and is safe from the audio thread
	for (auto& track : tracks)
	{
		track->peakL.store (0.0f);
		track->peakR.store (0.0f);
	}
}

void PlaybackEngine::clearOutputPeaks (void)
{
	outputPeakL.store (0.0f);
	outputPeakR.store (0.0f);
}

//==============================================================================
// song facts

void PlaybackEngine::refreshCachedTimes (void)
{
	if (song == nullptr)
	{
		cachedTimeline = {};
		return;
	}

	applyTimesTo (song->getInfo(), cachedTimeline, loopCount, fadeSeconds);
}

void PlaybackEngine::applyTimesTo (const playback::SongInfo& info,
								   playback::SongTimeline& timeline, int loops, double fade)
{
	// `loopCount` and `fadeSeconds` are passed in instead of read from `info`: the backend only
	// knows the song's own loop point, while how often the loop repeats and how long the fade is
	// are the engine's settings, and the mapping needs both sides.
	timeline = playback::makeSongTimeline (info.durationSeconds, info.singlePassSeconds,
										   info.loopLengthSeconds, loops, fade,
										   info.loopsForever);
}

double PlaybackEngine::getPlayLengthSeconds (void) const
{
	return (song != nullptr) ? song->getInfo().durationSeconds : 0.0;
}

juce::String PlaybackEngine::getTag (const juce::String& tagName) const
{
	// no lock: the map is only ever mutated from the message thread (load/reset) and the UI
	// polls it at 15 Hz - taking the render lock here would make the audio thread's try-lock
	// fail every now and then, dropping a whole block to silence
	auto it = tags.find (tagName);
	return (it == tags.end()) ? juce::String() : it->second;
}

//==============================================================================
// player bookkeeping (the lock is held by the caller)

void PlaybackEngine::updatePlayerTimeline (void)
{
	if (song != nullptr)
	{
		song->setTimeline (loopCount, fadeSeconds);		// the facts follow immediately
	}
	for (auto& player : trackPlayers)
	{
		player->setTimeline (loopCount, fadeSeconds);
	}
}

void PlaybackEngine::applyChipMutes (void)
{
	applyChipMutesTo (chips, trackPlayers);
}

void PlaybackEngine::applyChipMutesTo (const std::vector<ChipEntry>& chips,
									   const std::vector<std::unique_ptr<playback::ITrackPlayer>>& players)
{
	for (const auto& player : players)
	{
		for (const auto& chip : chips)
		{
			player->setChipAudible (chip.deviceId, ! chip.muted);
		}
	}
}

void PlaybackEngine::applyDefaultRouting (void)
{
	applyRoutingTo (defaultRouting, tracks);
}

void PlaybackEngine::applyRoutingTo (playback::DefaultRouting routing,
									 std::vector<std::unique_ptr<TrackState>>& tracks)
{
	// Track n goes to bus n under `spreadAcrossBuses`, everything to the master otherwise. The
	// scheme survives loading another song: buildSong() applies it to the fresh track states.
	for (size_t i = 0; i < tracks.size(); ++i)
	{
		tracks[i]->output.store (playback::defaultTargetForTrack (routing, (int) i));
	}
}

void PlaybackEngine::applyTrackIsolationTo (bool mixerEnabled,
											const std::vector<playback::TrackInfo>& layout,
											std::vector<std::unique_ptr<playback::ITrackPlayer>>& players)
{
	if (! mixerEnabled)
	{
		return;
	}

	for (size_t t = 0; t < players.size(); ++t)
	{
		if (t < layout.size())
		{
			players[t]->setTrackAudible (layout[t].id, true);
		}
	}
}

PlaybackEngine::LoadedSong PlaybackEngine::buildSong (const BuildConfig& config,
													  const juce::MemoryBlock& image,
													  const juce::File& file,
													  juce::String& errorMessage)
{
	// Static and self-contained on purpose: this runs while the engine is still playing the
	// previous song - and now also on a worker thread (review M-1) - so it may only look at the
	// configuration it was handed and the caller's copy of the chip list. It must not touch a
	// single member that render() can see. Everything it produces goes into the returned struct.
	LoadedSong pending;

	playback::SongMemory memory;
	memory.data = image.getData();
	memory.size = image.getSize();
	memory.file = file;

	// Compile-time registration (idempotent): the plugin ships its backends inside the same
	// binary, so there is nothing to load at runtime (ADR-008).
	playback::registerBuiltInBackends();
	auto* backend = playback::BackendRegistry::getInstance().findFor (memory);
	if (backend == nullptr)
	{
		errorMessage = "Unsupported file format.";
		return pending;
	}

	pending.song = backend->open (memory, errorMessage);
	if (pending.song == nullptr)
	{
		return pending;
	}

	// the slow part of a real load: unpacking a .vgz and starting every sound core
	if (! pending.song->prepare (config.sampleRate, config.maxBlockSize, errorMessage))
	{
		pending.song.reset();
		return pending;
	}

	pending.song->setTimeline (config.loopCount, config.fadeSeconds);
	pending.midiSequence = pending.song->getMidiSequence();

	// song facts
	pending.tags = pending.song->getInfo().tags;
	applyTimesTo (pending.song->getInfo(), pending.timeline, config.loopCount,
				  config.fadeSeconds);

	// chips, keeping the mute toggles when the same song is rebuilt (sample-rate change, mode
	// switch) - the caller passes the list the engine currently shows
	std::map<playback::DeviceId, bool> previousMutes;
	for (const auto& chip : config.chips)
	{
		previousMutes[chip.deviceId] = chip.muted;
	}

	for (const auto& chipInfo : pending.song->getInfo().chips)
	{
		ChipEntry chip;
		chip.name = chipInfo.name;
		chip.core = chipInfo.core;
		chip.deviceId = chipInfo.deviceId;

		if (const auto it = previousMutes.find (chip.deviceId); it != previousMutes.end())
		{
			chip.muted = it->second;
		}

		pending.chips.push_back (std::move (chip));
	}

	// the track list and one control state per track
	pending.layout = pending.song->getTracks();
	for (size_t i = 0; i < pending.layout.size(); ++i)
	{
		pending.tracks.push_back (std::make_unique<TrackState>());
	}

	// A new song inherits the routing scheme the user picked with the mixer card's shortcut
	// buttons - untouched, that is "everything to the master bus", the classic behaviour.
	applyRoutingTo (config.defaultRouting, pending.tracks);

	// One player per track when the backend can isolate them, a single un-isolated one otherwise
	// (plain player mode always uses a single player).
	const playback::BackendCapabilities caps = pending.song->getCapabilities();
	const bool perTrack = config.mixerEnabled
						   && caps.isolation == playback::IsolationMode::multiInstance
						   && ! pending.layout.empty();
	const size_t playersWanted = perTrack ? pending.layout.size() : 1;

	for (size_t i = 0; i < playersWanted; ++i)
	{
		auto player = pending.song->createPlayer (errorMessage);
		if (player == nullptr)
		{
			pending.players.clear();
			pending.song.reset();
			return pending;
		}

		player->start();
		pending.players.push_back (std::move (player));
	}

	if (perTrack)
	{
		applyTrackIsolationTo (config.mixerEnabled, pending.layout, pending.players);
	}
	applyChipMutesTo (pending.chips, pending.players);

	return pending;
}

void PlaybackEngine::installSong (LoadedSong&& built)
{
	// the only part of a load that has to be serialised against render(): pointer moves, the cached
	// facts and a handful of flags. The players of the outgoing song are destroyed here, with
	// everything else that goes out of scope - bounded work, unlike building them.
	song = std::move (built.song);
	trackPlayers = std::move (built.players);
	trackLayout = std::move (built.layout);
	tracks = std::move (built.tracks);
	soloCount = built.soloCount;
	chips = std::move (built.chips);
	tags = std::move (built.tags);
	midiSequence = built.midiSequence;
	midiCursor = 0;
	// Force one all-notes-off on the first block: a song swap must not leave notes from the
	// previous song hanging in the host.
	midiResetGeneration.fetch_add (1);
	midiSeenGeneration = midiResetGeneration.load() - 1;
	midiWasEnabled = false;
	midiLastBlockStart = -1.0;

	cachedTimeline = built.timeline;

	positionSeconds = 0.0;
	finished = false;
	loaded = true;
}

void PlaybackEngine::destroyPlayers (void)
{
	trackPlayers.clear();
}

void PlaybackEngine::rebuildPlayers (juce::String& errorMessage)
{
	const bool wasPlaying = playing.load();
	const double position = getPositionSeconds();

	// Snapshot first: the build must not read a member the audio thread or the UI can change
	// while it runs (review M-1). `chips` is the list on screen, so its mute toggles carry over.
	const BuildConfig config = makeBuildConfig();

	auto built = buildSong (config, fileData, currentFile, errorMessage);
	if (built.song == nullptr)
	{
		const juce::ScopedLock sl (lock);
		resetState();
		return;
	}

	{
		const juce::ScopedLock sl (lock);

		// A rebuild is the *same* song, so the mixer values the user set belong to its tracks and
		// have to survive it. The player objects are new (the old ones stay with the audio thread
		// until this swap), so the values are copied rather than the objects shared. The old code
		// got the same effect by keeping the track-state list across a rebuild.
		for (size_t i = 0; i < built.tracks.size() && i < tracks.size(); ++i)
		{
			built.tracks[i]->gain.store (tracks[i]->gain.load());
			built.tracks[i]->pan.store (tracks[i]->pan.load());
			built.tracks[i]->mute.store (tracks[i]->mute.load());
			built.tracks[i]->solo.store (tracks[i]->solo.load());
			built.tracks[i]->output.store (tracks[i]->output.load());
			built.tracks[i]->midiChannel.store (tracks[i]->midiChannel.load());
		}

		built.soloCount = 0;
		for (const auto& track : built.tracks)
		{
			if (track->solo.load())
			{
				++built.soloCount;
			}
		}

		applyTrackIsolationTo (mixerEnabled, built.layout, built.players);
		applyChipMutesTo (built.chips, built.players);

		installSong (std::move (built));
	}

	if (position > 0.0)
	{
		const juce::ScopedLock sl (lock);
		setPositionSecondsLocked (position);
	}
	if (! wasPlaying)
	{
		playing = false;	// stay where we were, paused or stopped
	}
}

void PlaybackEngine::resetState (void)
{
	playing = false;
	paused = false;
	finished = false;
	loaded = false;

	destroyPlayers();
	song.reset();
	trackLayout.clear();
	tracks.clear();
	soloCount = 0;
	midiSequence = nullptr;
	midiCursor = 0;
	midiWasEnabled = false;
	midiLastBlockStart = -1.0;

	fileData.reset();
	currentFile = juce::File();
	chips.clear();
	tags.clear();

	positionSeconds = 0.0;
	cachedTimeline = {};
}

//==============================================================================
// output routing (plain atomics - the audio thread reads them per block, no lock)

void PlaybackEngine::setTrackOutput (size_t trackIndex, playback::OutputTarget target)
{
	if (trackIndex >= tracks.size())
	{
		return;
	}

	// anything outside the declared buses falls back to the master bus, so the mix stays valid
	tracks[trackIndex]->output.store (playback::isRealOutputBus (target) ? target : playback::kMasterOutput);
}

playback::OutputTarget PlaybackEngine::getTrackOutput (size_t trackIndex) const
{
	return (trackIndex < tracks.size()) ? tracks[trackIndex]->output.load() : playback::kMasterOutput;
}

void PlaybackEngine::setTrackMidiChannel (size_t trackIndex, int channel)
{
	if (trackIndex < tracks.size())
	{
		tracks[trackIndex]->midiChannel.store (juce::jlimit (0, 16, channel));
		midiResetGeneration.fetch_add (1);
	}
}

int PlaybackEngine::getTrackMidiChannel (size_t trackIndex) const
{
	return (trackIndex < tracks.size()) ? tracks[trackIndex]->midiChannel.load() : 0;
}

void PlaybackEngine::setMasterOutput (playback::OutputTarget busIndex)
{
	// the master bus cannot target itself: anything invalid becomes bus 0 ("Out 1/2")
	masterOutput.store (playback::isRealOutputBus (busIndex) ? busIndex : 0);
}

void PlaybackEngine::setDefaultRouting (playback::DefaultRouting mode)
{
	// the lock guards the track list while it is walked; the targets themselves are atomics
	const juce::ScopedLock sl (lock);
	defaultRouting = mode;
	applyDefaultRouting();
}

bool PlaybackEngine::isOutputBusConnected (int busIndex) const
{
	if (busIndex < 0 || busIndex >= playback::kOutputBusCount)
	{
		return false;
	}

	return (connectedBuses.load() & ((std::uint64_t) 1 << busIndex)) != 0;
}

//==============================================================================
// audio thread

void PlaybackEngine::render (const playback::OutputBusBuffers* buses, int numBuses, int numSamples,
							 playback::MidiBlockSink* midiSink)
{
	// The block's timeline, in the order it has to happen - each step is a function of its own
	// below. None of them allocates; only the mixing steps touch the players (review M-1: this
	// function used to be all of them at once, 220 lines).
	const int busCount = juce::jmin (numBuses, playback::kOutputBusCount);

	publishBusConnectivity (buses, busCount);
	clearBusBuffers (buses, busCount, numSamples);

	// The DAW sync is looked at *before* the "is it playing" test below: following the host can be
	// what starts (or stops) the transport, and the early-out would never see a start.
	followDawTransport();

	// check the cheap atomics first: while paused or idle the audio thread must not touch
	// the lock at all (a failed try-lock costs a whole silent block). Following the host is the one
	// thing above that can have taken it already - and only while sync is on with a loaded song,
	// which is what followDawTransport() costs per block (review M-1).
	if (! playing.load() || ! loaded.load())
	{
		reportSilenceWhenIdle();
		return;
	}

	// The lock guards the players against the (rare, user-initiated) commands and against
	// (re)building them - the UI polling reads cached values instead, so this try-lock
	// succeeds while the song plays.
	const juce::ScopedTryLock stl (lock);
	if (! stl.isLocked() || trackPlayers.empty())
	{
		if (! stl.isLocked())
		{
			// A command held the lock across this block: we go silent instead of glitching, and this
			// counter is the only trace of it - the core test asserts a load does not cause it.
			starvedBlocks.fetch_add (1);
		}
		return;
	}

	const int usable = juce::jmin (numSamples, (int) mixLeft.size());
	if (usable <= 0)
	{
		return;
	}

	// The master bus accumulates its own copy - tracks routed to a real bus never pass through
	// it, so the master fader/mute/meter only ever see what was sent to the master.
	juce::FloatVectorOperations::clear (masterLeft.data(), usable);
	juce::FloatVectorOperations::clear (masterRight.data(), usable);

	// MIDI is generated from the same block start as the audio, before the players advance.
	if (midiSink != nullptr && ! trackPlayers.empty())
	{
		const double blockStart = trackPlayers[0]->positionSeconds();
		if (blockStart >= 0.0)
		{
			emitMidiForBlock (blockStart, usable, sampleRate, midiSink);
		}
	}

	if (mixerEnabled)
	{
		mixTracks (buses, busCount, usable);
	}
	else
	{
		renderPlainPlayer (usable);
	}

	finaliseMasterBus (buses, busCount, usable);
	applyOutputVolume (buses, busCount, numSamples);
	publishPositionAndEnd();
}

void PlaybackEngine::emitMidiForBlock (double blockStartSeconds, int numSamples, double hostSampleRate,
									   playback::MidiBlockSink* sink)
{
	const auto sendAllNotesOff = [this, sink] (int sampleOffset)
	{
		for (int channel = 0; channel < 16; ++channel)
		{
			sink->send ({ sampleOffset, (std::uint8_t) (0xB0 | channel), 123, 0 });
			sink->send ({ sampleOffset, (std::uint8_t) (0xB0 | channel), 120, 0 });
		}
	};

	if (! mixerEnabled)
	{
		if (midiWasEnabled)
		{
			sendAllNotesOff (0);
			midiWasEnabled = false;
		}
		return;
	}

	if (midiSequence == nullptr)
	{
		return;
	}

	const int generation = midiResetGeneration.load();
	if (generation != midiSeenGeneration
		|| (midiLastBlockStart >= 0.0 && blockStartSeconds + 0.001 < midiLastBlockStart))
	{
		sendAllNotesOff (0);
		midiSeenGeneration = generation;
		midiCursor = (std::size_t) (std::lower_bound (midiSequence->events.begin(),
													   midiSequence->events.end(),
													   blockStartSeconds,
													   [] (const playback::MidiEvent& event, double time)
													   {
														   return event.timeSeconds < time;
													   })
									 - midiSequence->events.begin());
	}

	midiLastBlockStart = blockStartSeconds;
	midiWasEnabled = true;

	const double blockEnd = blockStartSeconds + (double) numSamples / hostSampleRate;
	const int masterChannel = masterMidiChannel.load();
	const bool anySolo = soloCount.load() > 0;

	while (midiCursor < midiSequence->events.size())
	{
		const playback::MidiEvent& event = midiSequence->events[midiCursor];
		if (event.timeSeconds >= blockEnd)
		{
			break;
		}

		++midiCursor;
		if (event.timeSeconds < blockStartSeconds - 0.001)
		{
			continue;
		}

		if (event.sourceTrack >= tracks.size())
		{
			continue;
		}

		const TrackState& track = *tracks[event.sourceTrack];
		const bool audible = ! track.mute.load() && (! anySolo || track.solo.load());
		if (! audible)
		{
			continue;
		}

		const int channel = track.midiChannel.load();
		const int sampleOffset = juce::jlimit (0, juce::jmax (0, numSamples - 1),
											  (int) std::lround ((event.timeSeconds - blockStartSeconds)
																 * hostSampleRate));

		std::uint8_t status = 0;
		switch (event.type)
		{
		case playback::MidiEventType::noteOn:  status = 0x90; break;
		case playback::MidiEventType::noteOff: status = 0x80; break;
		case playback::MidiEventType::pitchBend: status = 0xE0; break;
		case playback::MidiEventType::controlChange: status = 0xB0; break;
		}

		if (status == 0)
		{
			continue;
		}

		if (channel > 0)
		{
			sink->send ({ sampleOffset, (std::uint8_t) (status | (channel - 1)),
						  event.data1, event.data2 });
		}

		// The master is an extra merged copy. Master mute silences that copy; if the direct
		// channel is the master channel, sending it twice would make the second note-off cut
		// the first note short.
		if (masterChannel > 0 && channel > 0 && masterChannel != channel && ! masterMuted.load())
		{
			sink->send ({ sampleOffset, (std::uint8_t) (status | (masterChannel - 1)),
						  event.data1, event.data2 });
		}
	}
}

void PlaybackEngine::publishBusConnectivity (const playback::OutputBusBuffers* buses, int busCount)
{
	// Publish which buses the host connected (the mixer card marks the others "not connected").
	// This runs before the early exits - the layout is valid while nothing plays, too.
	std::uint64_t connected = 0;
	for (int bus = 0; bus < busCount; ++bus)
	{
		if (buses[bus].isConnected())
		{
			connected |= (std::uint64_t) 1 << bus;
		}
	}
	connectedBuses.store (connected);
}

void PlaybackEngine::clearBusBuffers (const playback::OutputBusBuffers* buses, int busCount, int numSamples)
{
	// clear every connected bus first: the early exits and the mix path both leave the rest
	// of the block silent (a bus the host did not connect has no buffers to clear)
	for (int bus = 0; bus < busCount; ++bus)
	{
		if (buses[bus].left != nullptr)
		{
			juce::FloatVectorOperations::clear (buses[bus].left, numSamples);
		}
		if (buses[bus].right != nullptr)
		{
			juce::FloatVectorOperations::clear (buses[bus].right, numSamples);
		}
	}
}

void PlaybackEngine::reportSilenceWhenIdle (void)
{
	// No block publishes a level while nothing renders, so the meters would freeze at the
	// last block's peak - report silence instead. (The UI is not gated on this, it also
	// drops its meters when the transport is not playing; this keeps the engine's getters
	// truthful for every other reader.)
	if (! playing.load())
	{
		clearTrackPeaks();
		clearOutputPeaks();
	}
}

void PlaybackEngine::renderPlainPlayer (int usable)
{
	// the strips are inactive in plain player mode, so their meters stay at zero instead of
	// freezing at whatever they showed when the mixer was switched off
	clearTrackPeaks();

	// plain player mode is the master path on its own: the single player renders the whole
	// song, so switching the mixer off is "only the master bus outputs"
	playback::ITrackPlayer& player = *trackPlayers[0];
	player.render (masterLeft.data(), masterRight.data(), usable);
}

void PlaybackEngine::mixTracks (const playback::OutputBusBuffers* buses, int busCount, int usable)
{
	const bool anySolo = (soloCount.load() > 0);
	const size_t numTracks = juce::jmin (trackPlayers.size(), tracks.size());

	for (size_t t = 0; t < numTracks; ++t)
	{
		playback::ITrackPlayer& player = *trackPlayers[t];
		TrackState& state = *tracks[t];

		const int rendered = player.render (mixLeft.data(), mixRight.data(), usable);
		if (rendered <= 0)
		{
			state.peakL.store (0.0f);
			state.peakR.store (0.0f);
			continue;
		}

		const float gain = (state.mute.load() || (anySolo && ! state.solo.load()))
							 ? 0.0f : state.gain.load();
		const float pan = juce::jlimit (-1.0f, 1.0f, state.pan.load());
		// balance law: the centre keeps both sides at unity, panning attenuates the far side
		const float gainL = gain * (pan > 0.0f ? 1.0f - pan : 1.0f);
		const float gainR = gain * (pan < 0.0f ? 1.0f + pan : 1.0f);

		// where this track goes: the internal master bus, or straight to one output bus
		const playback::OutputTarget target = state.output.load();
		const bool toMaster = (target == playback::kMasterOutput);
		const bool toBus = ! toMaster
						   && playback::isRealOutputBus (target)
						   && target < busCount
						   && buses[target].isConnected();

		float* destL = toMaster ? masterLeft.data() : (toBus ? buses[target].left : nullptr);
		float* destR = toMaster ? masterRight.data() : (toBus ? buses[target].right : nullptr);

		float peakL = 0.0f;
		float peakR = 0.0f;
		for (int i = 0; i < rendered; ++i)
		{
			const float l = mixLeft[(size_t) i] * gainL;
			const float r = mixRight[(size_t) i] * gainR;

			// a track whose target bus is not connected is silent (the strip's dropdown
			// marks that bus as "not connected", so this is never a surprise)
			if (destL != nullptr)
			{
				destL[i] += l;
				destR[i] += r;
			}

			// the track meter keeps showing the track's own level, wherever it is routed
			peakL = juce::jmax (peakL, std::abs (l));
			peakR = juce::jmax (peakR, std::abs (r));
		}
		state.peakL.store (peakL);
		state.peakR.store (peakR);
	}
}

void PlaybackEngine::finaliseMasterBus (const playback::OutputBusBuffers* buses, int busCount, int usable)
{
	// --- the master bus: fader -> mute -> meter -> its target bus -----------------------------
	// the mixer master (the master strip's fader): a mix-time gain on the master sum
	const float mixerMaster = mixerMasterGain.load();
	if (mixerMaster != 1.0f)
	{
		juce::FloatVectorOperations::multiply (masterLeft.data(), mixerMaster, usable);
		juce::FloatVectorOperations::multiply (masterRight.data(), mixerMaster, usable);
	}

	if (masterMuted.load())
	{
		// muting the master silences only what the master contributes - tracks on real buses
		// keep playing, and the master meter (which measures the master bus) reads zero
		juce::FloatVectorOperations::clear (masterLeft.data(), usable);
		juce::FloatVectorOperations::clear (masterRight.data(), usable);
	}

	// the master meter: the level of the master bus, read lock-free by the UI
	float peakL = 0.0f;
	float peakR = 0.0f;
	for (int i = 0; i < usable; ++i)
	{
		peakL = juce::jmax (peakL, std::abs (masterLeft[(size_t) i]));
		peakR = juce::jmax (peakR, std::abs (masterRight[(size_t) i]));
	}
	outputPeakL.store (peakL);
	outputPeakR.store (peakR);

	const playback::OutputTarget masterTarget = masterOutput.load();
	const bool masterTargetConnected = buses != nullptr
										&& playback::isRealOutputBus (masterTarget)
										&& masterTarget < busCount
										&& buses[masterTarget].isConnected();

	if (masterTargetConnected)
	{
		float* destL = buses[masterTarget].left;
		float* destR = buses[masterTarget].right;
		for (int i = 0; i < usable; ++i)
		{
			destL[i] += masterLeft[(size_t) i];
			destR[i] += masterRight[(size_t) i];
		}
	}
}

void PlaybackEngine::applyOutputVolume (const playback::OutputBusBuffers* buses, int busCount, int numSamples)
{
	// The output volume (the transport card's slider) is the very last stage and applies to
	// every bus - it is the plugin's own output level, so direct outputs are not exempt.
	const float outputVolume = masterVolume.load();
	if (outputVolume != 1.0f)
	{
		for (int bus = 0; bus < busCount; ++bus)
		{
			if (buses[bus].left != nullptr)
			{
				juce::FloatVectorOperations::multiply (buses[bus].left, outputVolume, numSamples);
			}
			if (buses[bus].right != nullptr)
			{
				juce::FloatVectorOperations::multiply (buses[bus].right, outputVolume, numSamples);
			}
		}
	}
}

void PlaybackEngine::publishPositionAndEnd (void)
{
	// publish the position for the lock-free UI getter (an O(1) query, still under the lock);
	// the first player carries the song timeline in both modes
	const double position = trackPlayers[0]->positionSeconds();
	if (position >= 0.0)
	{
		positionSeconds.store (position);
	}

	if (trackPlayers[0]->isFinished())
	{
		finished = true;
		playing = false;	// the song is over -> silence
		paused = false;
	}
}

} // namespace toybox
