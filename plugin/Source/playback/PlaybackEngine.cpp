#include "PlaybackEngine.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include "playback/BackendRegistry.h"

namespace toybox
{

PlaybackEngine::PlaybackEngine (void)
{
}

PlaybackEngine::~PlaybackEngine (void)
{
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
	// Phase 1, **outside the lock**: reading the file. This is the one part of a load whose cost is
	// unbounded - a 30 MB VGM from a cold cache, or a file something else is still writing - and it
	// touches nothing this engine owns, so the audio thread keeps rendering through it instead of
	// falling silent for as long as the disk takes (the review's H-1; the rest of the load is
	// bounded work that still happens under the lock, see the note there).
	const bool exists = file.existsAsFile();
	juce::MemoryBlock data;
	const bool readable = exists && file.loadFileAsData (data) && data.getSize() > 0;

	// Phase 2, still outside the lock: unpacking and starting the sound cores, into a song of its
	// own. The current song keeps playing while this happens - a failed load still stops playback
	// below, exactly as it always has. (`chips` is message-thread data, like everything else read
	// here; render() never touches it.)
	LoadedSong built;
	if (readable)
	{
		built = buildSong (data, file, chips, errorMessage);
	}

	// Phase 3, under the lock: the swap, and nothing else.
	const juce::ScopedLock sl (lock);

	resetState();	// stop playback and release the previous file first

	if (! readable)
	{
		errorMessage = exists ? "Could not read the file." : "The file does not exist.";
		return false;
	}

	if (built.song == nullptr)
	{
		return false;	// errorMessage was filled in by the backend
	}

	// the backend unpacks a compressed container (.vgz) itself - the image stays as it was read
	fileData.swapWith (data);
	currentFile = file;

	installSong (std::move (built));

	positionSeconds = 0.0;
	finished = false;
	playing = true;

	return true;
}

void PlaybackEngine::unloadFile (void)
{
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
}

void PlaybackEngine::pause (void)
{
	playing = false;
}

void PlaybackEngine::stop (void)
{
	playing = false;
	finished = false;

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
		cachedDurationSeconds = 0.0;
		cachedLoopPointSeconds = -1.0;
		loopsForever = false;
		return;
	}

	applyTimesTo (song->getInfo(), cachedDurationSeconds, cachedLoopPointSeconds, loopsForever);
}

void PlaybackEngine::applyTimesTo (const playback::SongInfo& info, double& durationSeconds,
								   double& loopPointSeconds, bool& loopsForever)
{
	durationSeconds = info.durationSeconds;
	loopPointSeconds = info.loopPointSeconds;
	loopsForever = info.loopsForever;
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

PlaybackEngine::LoadedSong PlaybackEngine::buildSong (const juce::MemoryBlock& image, const juce::File& file,
													 const std::vector<ChipEntry>& previousChips,
													 juce::String& errorMessage) const
{
	// Read-only on purpose: this runs while the engine is still playing the previous song, so it
	// may look at the configuration (sample rate, block size, mixer mode, loop/fade, routing) and
	// at the caller's copy of the chip list, but it must not touch a single member that render()
	// can see. Everything it produces goes into the returned struct.
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
	if (! pending.song->prepare (sampleRate, maxBlockSize, errorMessage))
	{
		pending.song.reset();
		return pending;
	}

	pending.song->setTimeline (loopCount, fadeSeconds);

	// song facts
	pending.tags = pending.song->getInfo().tags;
	applyTimesTo (pending.song->getInfo(), pending.durationSeconds, pending.loopPointSeconds,
				  pending.loopsForever);

	// chips, keeping the mute toggles when the same song is rebuilt (sample-rate change, mode
	// switch) - the caller passes the list the engine currently shows
	std::map<playback::DeviceId, bool> previousMutes;
	for (const auto& chip : previousChips)
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
	applyRoutingTo (defaultRouting, pending.tracks);

	// One player per track when the backend can isolate them, a single un-isolated one otherwise
	// (plain player mode always uses a single player).
	const playback::BackendCapabilities caps = pending.song->getCapabilities();
	const bool perTrack = mixerEnabled
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
		applyTrackIsolationTo (mixerEnabled, pending.layout, pending.players);
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

	cachedDurationSeconds = built.durationSeconds;
	cachedLoopPointSeconds = built.loopPointSeconds;
	loopsForever = built.loopsForever;

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
	const std::vector<ChipEntry> previousChips = chips;		// message-thread data

	auto built = buildSong (fileData, currentFile, previousChips, errorMessage);
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
		playing = false;	// stay paused where we were
	}
}

void PlaybackEngine::resetState (void)
{
	playing = false;
	finished = false;
	loaded = false;

	destroyPlayers();
	song.reset();
	trackLayout.clear();
	tracks.clear();
	soloCount = 0;

	fileData.reset();
	currentFile = juce::File();
	chips.clear();
	tags.clear();

	positionSeconds = 0.0;
	cachedDurationSeconds = 0.0;
	cachedLoopPointSeconds = -1.0;
	loopsForever = false;
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

void PlaybackEngine::render (const playback::OutputBusBuffers* buses, int numBuses, int numSamples)
{
	const int busCount = juce::jmin (numBuses, playback::kOutputBusCount);

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

	// check the cheap atomics first: while paused or idle the audio thread must not touch
	// the lock at all (a failed try-lock costs a whole silent block)
	if (! playing.load() || ! loaded.load())
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

	const playback::OutputTarget masterTarget = masterOutput.load();
	const bool masterTargetConnected = buses != nullptr
										&& playback::isRealOutputBus (masterTarget)
										&& masterTarget < busCount
										&& buses[masterTarget].isConnected();

	if (! mixerEnabled)
	{
		// the strips are inactive in plain player mode, so their meters stay at zero instead of
		// freezing at whatever they showed when the mixer was switched off
		clearTrackPeaks();

		// plain player mode is the master path on its own: the single player renders the whole
		// song, so switching the mixer off is "only the master bus outputs"
		playback::ITrackPlayer& player = *trackPlayers[0];
		player.render (masterLeft.data(), masterRight.data(), usable);
	}
	else
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
	}
}

} // namespace toybox
